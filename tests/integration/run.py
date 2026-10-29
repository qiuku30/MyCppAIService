#!/usr/bin/env python3
"""Live HTTP/MySQL/RabbitMQ integration; Python standard library only.

This runner never creates, builds, or removes containers/volumes. Lifecycle stages
operate only on the explicitly named, integration-labelled containers. Receipts
exclude generated passwords, login cookies and environment credentials.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import http.client
import json
import os
from pathlib import Path
import re
import secrets
import socket
import subprocess
import sys
import time
import urllib.parse


STAGES = ("prepare", "http", "auth", "producer", "persistence", "restart", "db-restart", "broker", "gemma")
LABEL = "io.mycpp.integration"


class Unavailable(Exception):
    pass


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def digest(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True,
                                     separators=(",", ":")).encode()).hexdigest()


def sql_text(value):
    return "CONVERT(X'" + str(value).encode().hex() + "' USING utf8mb4)"


def write_private(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".new")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as output:
        json.dump(value, output, ensure_ascii=False, indent=2)
        output.write("\n")
    os.chmod(temporary, 0o600)
    temporary.replace(path)


class Runner:
    def __init__(self, args):
        self.args = args
        self.url = urllib.parse.urlsplit(args.base_url)
        require(self.url.scheme == "http" and self.url.hostname in ("127.0.0.1", "localhost", "::1"),
                "BASE_URL must be a loopback HTTP URL for the isolated test stack")
        require(not self.url.path.strip("/") and not self.url.username,
                "BASE_URL must not contain a path or credentials")
        require(re.fullmatch(r"mycpp_it(?:_[A-Za-z0-9_]+)?", args.db_name) is not None,
                "IT_DB_NAME must be mycpp_it or have the mycpp_it_ prefix")
        self.containers = {
            "mysql": args.mysql_container, "rabbitmq": args.rabbit_container,
            "app": args.app_container, "control": args.control_container,
        }
        for name in self.containers.values():
            require(re.fullmatch(r"mycpp-it-[A-Za-z0-9_.-]+", name) is not None,
                    "Container names must use the mycpp-it- prefix")
        self.state_path = Path(args.state)
        self.state = json.loads(self.state_path.read_text()) if self.state_path.exists() else {
            "run_id": time.strftime("%Y%m%dT%H%M%S", time.gmtime()) + "-" + secrets.token_hex(4),
            "accounts": {}, "exchanges": [], "legacy": [],
        }
        self.receipts = []
        self.checks = []
        self.validated = set()

    def save(self):
        write_private(self.state_path, self.state)

    def docker(self, *arguments, stdin=None, timeout=90):
        command = [self.args.docker]
        if self.args.docker_context:
            command += ["--context", self.args.docker_context]
        command += list(arguments)
        try:
            result = subprocess.run(command, input=stdin, capture_output=True,
                                    text=True, timeout=timeout, check=False)
        except FileNotFoundError as error:
            raise Unavailable("Docker CLI unavailable") from error
        except subprocess.TimeoutExpired as error:
            raise RuntimeError("Docker operation timed out") from error
        if result.returncode:
            # Docker output/environment/command lines may contain credentials.
            raise RuntimeError("Docker " + str(arguments[0]) + " failed with exit " + str(result.returncode))
        return result.stdout

    def inspect(self, kind):
        name = self.containers[kind]
        data = json.loads(self.docker("inspect", name))[0]
        require(data.get("Config", {}).get("Labels", {}).get(LABEL) == "true",
                "Refusing unlabelled container: " + name)
        self.validated.add(kind)
        return data

    def exec(self, kind, script, stdin=None, timeout=90):
        if kind not in self.validated:
            self.inspect(kind)
        return self.docker("exec", "-i", self.containers[kind], "sh", "-eu", "-c", script,
                           stdin=stdin, timeout=timeout)

    def lifecycle(self, operation, kind):
        self.inspect(kind)
        require(operation in ("restart", "stop", "start"), "Unsupported lifecycle operation")
        self.docker(operation, self.containers[kind], timeout=180)

    def db(self, query):
        script = ('export MYSQL_PWD="$MYSQL_ROOT_PASSWORD"; '
                  'exec mysql --default-character-set=utf8mb4 --batch --raw --skip-column-names '
                  '--user=root --database=' + self.args.db_name)
        output = self.exec("mysql", script, stdin=query + ";\n")
        return [json.loads(line) for line in output.splitlines() if line.strip()]

    def scalar(self, expression):
        values = self.db("SELECT JSON_OBJECT('value', (" + expression + "))")
        require(len(values) == 1, "Database scalar returned an unexpected row count")
        return values[0]["value"]

    def broker(self, method, path, body=None):
        payload = "" if body is None else json.dumps(body, ensure_ascii=False, separators=(",", ":"))
        payload64 = base64.b64encode(payload.encode()).decode()
        # Only the container reads credentials. No host command contains them.
        script = ('printf %s ' + payload64 + ' | base64 -d | '
                  'curl --silent --show-error --fail --max-time 20 '
                  '--user "$RABBITMQ_USER:$RABBITMQ_PASS" '
                  '--header "Content-Type: application/json" --request ' + method +
                  ' --data-binary @- "http://$RABBITMQ_HOST:15672/api' + path + '"')
        result = self.exec("control", script, timeout=30)
        return json.loads(result) if result.strip() else None

    def publish(self, payload, queue="sql_queue"):
        body = payload if isinstance(payload, str) else json.dumps(payload, ensure_ascii=False)
        result = self.broker("POST", "/exchanges/%2F/amq.default/publish", {
            "properties": {"delivery_mode": 2, "content_type": "application/json"},
            "routing_key": queue, "payload": body, "payload_encoding": "string",
        })
        require(result and result.get("routed") is True, "Real broker did not route published message")

    def queue(self, name="sql_queue"):
        return self.broker("GET", "/queues/%2F/" + urllib.parse.quote(name, safe=""))

    def wait_queue_idle(self):
        def idle():
            queue = self.queue()
            return queue.get("messages") == 0 and queue.get("messages_unacknowledged") == 0
        self.wait(idle, "all real queue deliveries to be consumed and acknowledged")

    def peek(self, name="sql_queue", count=10):
        return self.broker("POST", "/queues/%2F/" + urllib.parse.quote(name, safe="") + "/get", {
            "count": count, "ackmode": "ack_requeue_true", "encoding": "auto", "truncate": 65536,
        })

    def wait(self, predicate, description, timeout=90):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                value = predicate()
                if value:
                    return value
            except (RuntimeError, OSError, http.client.HTTPException):
                pass
            time.sleep(1)
        raise AssertionError("Timed out waiting for " + description)

    def request(self, method, path, payload=None, cookie=None, headers=None, timeout=None):
        connection = http.client.HTTPConnection(self.url.hostname, self.url.port or 80,
                                                 timeout=timeout or self.args.timeout)
        data = None if payload is None else json.dumps(payload, ensure_ascii=False).encode()
        request_headers = {"Connection": "close"}
        if payload is not None:
            request_headers["Content-Type"] = "application/json"
        if cookie:
            request_headers["Cookie"] = cookie
        request_headers.update(headers or {})
        try:
            connection.request(method, path, body=data, headers=request_headers)
            response = connection.getresponse()
            body = response.read()
            return response.status, {k.lower(): v for k, v in response.getheaders()}, body
        finally:
            connection.close()

    def api(self, method, path, payload=None, account=None, expected=200, headers=None, timeout=None):
        cookie = account.get("cookie") if isinstance(account, dict) else account
        status, response_headers, body = self.request(method, path, payload, cookie, headers, timeout)
        require(status == expected, path + " expected HTTP " + str(expected) + ", got " + str(status))
        try:
            value = json.loads(body) if body else None
        except ValueError as error:
            raise AssertionError(path + " returned invalid JSON") from error
        return value, response_headers

    def check(self, name, action):
        result = action()
        item = {"name": name, "status": "passed"}
        if result is not None:
            item["evidence"] = result
        self.checks.append(item)
        return result

    def wait_http(self):
        return self.wait(lambda: self.request("GET", "/__integration_ready__")[0] == 404,
                         "application HTTP readiness", self.args.restart_timeout)

    def read_response(self, stream):
        status_line = stream.readline(8192)
        require(re.fullmatch(rb"HTTP/1\.[01] [1-5][0-9][0-9] [^\r\n]+\r\n", status_line) is not None,
                "Invalid HTTP status line")
        status = int(status_line.split(b" ", 2)[1])
        headers = {}
        while True:
            line = stream.readline(8192)
            require(bool(line), "Response headers ended before CRLF separator")
            if line == b"\r\n":
                break
            require(b":" in line and line.endswith(b"\r\n"), "Invalid response header framing")
            key, value = line[:-2].split(b":", 1)
            headers[key.decode().lower()] = value.decode().strip()
        require("transfer-encoding" not in headers, "Unexpected transfer coding in framed response")
        # These statuses end at the header separator. A 204 must not carry
        # Content-Length; a 304 may describe a representation but has no body.
        if status in (204, 304):
            if status == 204:
                require("content-length" not in headers, "204 must not include Content-Length")
            elif "content-length" in headers:
                require(re.fullmatch(r"[0-9]+", headers["content-length"]) is not None,
                        "304 has an invalid Content-Length")
            return status, headers, b""
        require(re.fullmatch(r"[0-9]+", headers.get("content-length", "")) is not None,
                "Response has no valid Content-Length")
        length = int(headers["content-length"])
        body = stream.read(length)
        require(len(body) == length, "Response body shorter than Content-Length")
        return status, headers, body

    def raw(self, payload, count=1, fragment=None, expect_eof=False):
        with socket.create_connection((self.url.hostname, self.url.port or 80), self.args.timeout) as conn:
            conn.settimeout(self.args.timeout)
            if fragment is None:
                conn.sendall(payload)
            else:
                conn.sendall(payload[:fragment])
                conn.settimeout(0.15)
                try:
                    early = conn.recv(1)
                except socket.timeout:
                    early = None
                require(early is None, "Server responded to an incomplete request")
                conn.settimeout(self.args.timeout)
                conn.sendall(payload[fragment:])
            stream = conn.makefile("rb")
            try:
                responses = [self.read_response(stream) for _ in range(count)]
                if expect_eof:
                    require(stream.read(1) == b"", "Terminal response processed a pipelined request")
                return responses
            finally:
                stream.close()

    def prepare(self):
        evidence = []
        for kind in self.containers:
            data = self.inspect(kind)
            require(data.get("State", {}).get("Running"), "Container is not running: " + kind)
            bindings = data.get("HostConfig", {}).get("PortBindings") or {}
            if kind != "app":
                require(not bindings, "Database/broker/control ports must not be exposed on the host")
            else:
                require(bindings and all(b["HostIp"] in ("127.0.0.1", "::1")
                                        for values in bindings.values() for b in values),
                        "Application HTTP must be bound only to loopback")
            evidence.append({"container": self.containers[kind], "id": data["Id"][:12],
                             "image_id": data["Image"], "integration_label": True})
        self.check("isolated_container_identity", lambda: evidence)
        tables = self.db("SELECT JSON_OBJECT('table',TABLE_NAME) FROM information_schema.tables "
                         "WHERE TABLE_SCHEMA=" + sql_text(self.args.db_name))
        require({x["table"] for x in tables} >= {"users", "chat_message"}, "Real database schema missing")
        self.check("real_mysql_schema", lambda: {"database": self.args.db_name, "tables": sorted(x["table"] for x in tables)})
        queue = self.wait(lambda: self.queue(), "real RabbitMQ queue")
        require(queue["durable"] is True, "Application broker queue is not durable")
        self.check("real_rabbitmq_queue", lambda: {"queue": "sql_queue", "durable": True})
        self.wait_http()

    def http(self):
        self.wait_http()
        prefix = b"GET /__integration_missing__ HTTP/1.1\r\nHost: localhost\r\n"
        self.check("default_404_status_and_framing", lambda: self.expect_raw(prefix + b"Connection: close\r\n\r\n", [404]))
        cors = b"OPTIONS /login HTTP/1.1\r\nHost: localhost\r\noRiGiN: http://integration.invalid\r\nConnection: close\r\n\r\n"
        def cors_check():
            response = self.raw(cors)[0]
            require(response[0] == 204, "CORS preflight did not return 204")
            require(response[1].get("access-control-allow-origin") == "http://integration.invalid", "CORS origin lost")
            return {"status": 204, "body_bytes": len(response[2])}
        self.check("cors_204_case_insensitive_header", cors_check)
        invalid_headers = {
            "negative_length": b"Content-Length: -1", "signed_length": b"Content-Length: +1",
            "trailing_length_text": b"Content-Length: 1x", "empty_length": b"Content-Length:",
            "overflow_length": b"Content-Length: 184467440737095516160",
            "body_limit": b"Content-Length: 10485761",
            "conflicting_lengths": b"Content-Length: 0\r\ncOnTeNt-LeNgTh: 1",
            "unsupported_transfer_coding": b"Transfer-Encoding: chunked",
            "ambiguous_framing": b"Content-Length: 0\r\nTransfer-Encoding: chunked",
            "whitespace_in_header_name": b"Content-Length : 0",
            "header_without_colon": b"BrokenHeader", "control_in_header_value": b"X-Test: a\x01b",
        }
        for name, header in invalid_headers.items():
            self.check("reject_" + name, lambda header=header: self.expect_raw(prefix + header + b"\r\nConnection: close\r\n\r\n", [400]))
        same = b"OPTIONS /login HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\ncOnTeNt-LeNgTh: 0\r\nConnection: close\r\n\r\n"
        self.check("identical_duplicate_lengths", lambda: self.expect_raw(same, [204]))
        first = b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
        second = prefix + b"Connection: close\r\n\r\n"
        self.check("coalesced_two_requests", lambda: self.expect_raw(first + second, [200, 404]))
        self.check("terminal_close_stops_coalesced_tail",
                   lambda: self.expect_raw(cors + second, [204], expect_eof=True))
        self.check("fragmented_request_waits_for_completion", lambda: self.expect_raw(second, [404], fragment=17))
        login = json.dumps({"username": "it_missing_" + self.state["run_id"], "password": "unused"}).encode()
        login_request = (b"POST /login HTTP/1.1\r\nHost: localhost\r\ncOnTeNt-TyPe: application/json\r\ncOnTeNt-LeNgTh: " +
                         str(len(login)).encode() + b"\r\n\r\n" + login)
        empty_login = b"POST /login HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
        self.check("coalesced_body_reset_and_mixed_case_headers", lambda: self.expect_raw(login_request + empty_login, [401, 400]))
        self.save()

    def expect_raw(self, payload, expected, fragment=None, expect_eof=False):
        result = self.raw(payload, len(expected), fragment, expect_eof)
        statuses = [x[0] for x in result]
        require(statuses == expected, "Raw HTTP expected " + str(expected) + ", got " + str(statuses))
        return {"statuses": statuses, "body_bytes": [len(x[2]) for x in result]}

    def login(self, account):
        value, headers = self.api("POST", "/login", {"username": account["username"], "password": account["password"]})
        require(value.get("success") is True and value.get("userId") == account["id"], "Login identity mismatch")
        cookie = headers.get("set-cookie", "").split(";", 1)[0]
        require(re.fullmatch(r"sessionId=[0-9a-f]{32}", cookie) is not None, "Login did not issue a session cookie")
        account["cookie"] = cookie
        self.save()

    def accounts(self):
        require({"a", "b", "literal"} <= self.state["accounts"].keys(), "Run auth stage first with the same --state")
        return self.state["accounts"]

    def auth(self):
        shared_password = self.state["accounts"].get("a", {}).get("password") or secrets.token_urlsafe(24)
        suffixes = {"a": "a", "b": "b", "literal": "x' OR '1'='1"}
        for key, suffix in suffixes.items():
            account = self.state["accounts"].get(key)
            if account is None:
                account = {"username": "it_" + self.state["run_id"] + "_" + suffix,
                           "password": shared_password}
                value, _ = self.api("POST", "/register", account)
                require(value.get("status") == "success" and int(value.get("userId", 0)) > 0, "Registration failed")
                account["id"] = int(value["userId"])
                self.state["accounts"][key] = account
                self.save()
            self.check("registered_" + key, lambda account=account: {"user_id": account["id"]})
            self.login(account)
            self.check("login_" + key, lambda: {"authenticated": True})
        accounts = self.accounts()
        self.api("POST", "/register", {"username": accounts["a"]["username"], "password": shared_password}, expected=409)
        self.check("duplicate_registration_rejected", lambda: {"status": 409})
        self.api("POST", "/login", {"username": accounts["a"]["username"], "password": "incorrect-" + shared_password}, expected=401)
        self.check("wrong_password_rejected", lambda: {"status": 401})
        self.api("POST", "/login", {"username": "unregistered' OR '1'='1", "password": "wrong"}, expected=401)
        self.check("sql_expression_does_not_authenticate", lambda: {"status": 401})
        hashes = []
        for account in accounts.values():
            rows = self.db("SELECT JSON_OBJECT('id',id,'username',username,'password',password) FROM users WHERE username=" + sql_text(account["username"]))
            require(len(rows) == 1 and rows[0]["id"] == account["id"] and rows[0]["username"] == account["username"], "Stored account identity differs from literal username")
            encoded = rows[0]["password"]
            match = re.fullmatch(r"pbkdf2_sha256\$([0-9]+)\$([0-9a-f]{32})\$([0-9a-f]{64})", encoded)
            require(match is not None and encoded != account["password"], "Stored password is not a salted PBKDF2 hash")
            cost, salt, key = match.groups()
            derived = hashlib.pbkdf2_hmac("sha256", account["password"].encode(), bytes.fromhex(salt), int(cost)).hex()
            require(hmac.compare_digest(derived, key), "Stored hash does not verify independently")
            hashes.append(encoded)
        require(len(set(hashes)) == len(hashes), "Accounts reused the same password salt")
        self.check("real_mysql_salted_hash_and_literal_username", lambda: {"accounts": len(hashes), "independent_pbkdf2_verification": True, "distinct_salted_encodings": True})
        for path in ("/chat/history", "/chat/send"):
            self.api("POST", path, {"sessionId": "missing", "question": "ownership probe", "modelType": "5"}, expected=401)
        self.api("GET", "/chat/sessions", expected=401)
        self.check("unauthenticated_chat_routes_rejected", lambda: {"routes": 3, "status": 401})

    def new_exchange(self, key, label):
        account = self.accounts()[key]
        return {"v": 1, "message_id": secrets.token_hex(16), "user_id": account["id"],
                "username": account["username"], "session_id": secrets.token_hex(16),
                "question": "integration " + label + ": O'Reilly; SELECT 'literal'\\n 中文\nnext line",
                "answer": "real broker input " + label + ": quotation ' and \\\\ preserved; 回答",
                "ts": int(time.time() * 1000)}

    def exchange_rows(self, exchange):
        return self.db("SELECT JSON_OBJECT('message_id',message_id,'id',id,'username',username,'session_id',session_id,'is_user',is_user,'content',content,'ts',ts) FROM chat_message WHERE message_id IN (" +
                       sql_text(exchange["message_id"] + "/u") + "," + sql_text(exchange["message_id"] + "/a") + ") ORDER BY ts,message_id")

    def expected_rows(self, exchange):
        base = {"id": exchange["user_id"], "username": exchange["username"], "session_id": exchange["session_id"]}
        return [dict(base, message_id=exchange["message_id"] + suffix, is_user=role, content=content, ts=exchange["ts"] + offset)
                for suffix, role, content, offset in (("/u", 1, exchange["question"], 0), ("/a", 0, exchange["answer"], 1))]

    def wait_exchange(self, exchange):
        expected = self.expected_rows(exchange)
        return self.wait(lambda: self.exchange_rows(exchange) == expected, "exact real database exchange persistence")

    def add_exchange(self, exchange):
        if exchange["message_id"] not in {x["message_id"] for x in self.state["exchanges"]}:
            self.state["exchanges"].append(exchange)
            self.save()

    def snapshot(self):
        ids = ",".join(str(x["id"]) for x in self.accounts().values())
        return self.db("SELECT JSON_OBJECT('message_id',message_id,'id',id,'username',username,'session_id',session_id,'is_user',is_user,'content',content,'ts',ts) FROM chat_message WHERE id IN (" + ids + ") ORDER BY id,session_id,ts,content")

    def persistence(self):
        self.accounts()
        existing = {x.get("label"): x for x in self.state["exchanges"]}
        for key, label in (("a", "mq-a"), ("b", "mq-b")):
            exchange = existing.get(label) or self.new_exchange(key, label)
            exchange["label"] = label
            self.add_exchange(exchange)
            for _ in range(3):
                self.publish({k: v for k, v in exchange.items() if k != "label"})
            self.wait_exchange(exchange)
            self.wait_queue_idle()
            require(self.exchange_rows(exchange) == self.expected_rows(exchange), "Duplicate delivery changed persisted exchange")
            self.check("structured_exchange_exact_and_deduplicated_" + key,
                       lambda exchange=exchange: {"message_id": exchange["message_id"], "rows": 2,
                                                  "sha256": digest(self.exchange_rows(exchange)), "publications": 3,
                                                  "provenance": "real_rabbitmq_management_api"})
        if not self.state["legacy"]:
            account = self.accounts()["a"]
            sid = secrets.token_hex(16)
            timestamp = int(time.time() * 1000)
            self.state["legacy"] = [{"id": account["id"], "username": account["username"],
                                     "session_id": sid, "is_user": role,
                                     "content": "legacy SQL seeded role " + str(role) + " row " + str(index) + ": 空白\n'quoted'",
                                     "ts": timestamp + index} for index, role in enumerate((0, 1, 1))]
            self.save()
        for row in self.state["legacy"]:
            count = self.scalar("SELECT COUNT(*) FROM chat_message WHERE id=" + str(row["id"]) + " AND session_id=" + sql_text(row["session_id"]) + " AND ts=" + str(row["ts"]))
            if count == 0:
                self.db("INSERT INTO chat_message (message_id,id,username,session_id,is_user,content,ts) VALUES (NULL," +
                        str(row["id"]) + "," + sql_text(row["username"]) + "," + sql_text(row["session_id"]) + "," +
                        str(row["is_user"]) + "," + sql_text(row["content"]) + "," + str(row["ts"]) + ")")
        self.check("legacy_history_metadata_seeded_in_real_mysql", lambda: {"rows": len(self.state["legacy"]), "roles": [0, 1, 1], "provenance": "explicit_synthetic_legacy_sql_rows"})
        probe_name = "it_quarantine_" + self.state["run_id"]
        invalid = "INSERT INTO users(username,password) VALUES ('" + probe_name + "','quarantine_probe_only')"
        self.publish(invalid)
        def quarantined():
            return any(item.get("payload") == invalid for item in self.peek("sql_queue.dead", 100))
        self.wait(quarantined, "legacy SQL payload quarantine")
        self.wait_queue_idle()
        require(self.scalar("SELECT COUNT(*) FROM users WHERE username=" + sql_text(probe_name)) == 0, "Legacy SQL broker payload was executed")
        self.check("legacy_sql_is_quarantined_and_not_executed", lambda: {"payload_sha256": digest(invalid), "quarantined": True, "executed_rows": 0})
        self.state["snapshot_sha256"] = digest(self.snapshot())
        self.save()

    def producer(self):
        """Exercise current production MQManager, not a management publication."""
        self.accounts()
        queue_before = self.queue()
        publish_before = queue_before.get("message_stats", {}).get("publish", 0)
        exchange = self.new_exchange("a", "real-MQManager-producer")
        self.add_exchange(exchange)
        require(1 <= self.args.producer_seconds <= 60, "--producer-seconds must be between 1 and 60")
        script = ("sh /workspace/tests/integration/mq_producer.sh --copies 3 --seconds " +
                  str(float(self.args.producer_seconds)))
        try:
            output = self.exec("app", script, stdin=json.dumps(exchange, ensure_ascii=False),
                               timeout=self.args.producer_seconds + 180)
        except RuntimeError as error:
            raise Unavailable("Real MQManager driver compile/execution unavailable; inspect /workspace/build/it-mq-producer-build.log; no producer pass") from error
        summaries = []
        for line in output.splitlines():
            try:
                item = json.loads(line)
            except ValueError:
                continue  # Third-party runtime logs are not evidence receipts.
            if isinstance(item, dict) and item.get("driver") == "real_MQManager":
                summaries.append(item)
        if len(summaries) != 1:
            raise Unavailable("Real MQManager driver did not produce one valid admission summary")
        summary = summaries[0]
        if summary.get("status") != "admitted":
            raise Unavailable("Real MQManager driver unavailable; no producer pass")
        require(summary.get("message_id") == exchange["message_id"] and
                summary.get("requested") == 3 and summary.get("admitted") == 3 and
                summary.get("memory_admission_only") is True and
                summary.get("publisher_confirms_observed") is False,
                "Real MQManager driver admission summary differs from submitted exchange")
        self.check("real_MQManager_memory_admission_only", lambda: summary)
        self.wait_exchange(exchange)
        queue_after = self.wait(lambda: self.producer_publications(publish_before),
                                "three real MQManager publications in broker queue statistics")
        self.wait_queue_idle()
        rows = self.exchange_rows(exchange)
        require(rows == self.expected_rows(exchange), "Real producer duplicate delivery changed persisted exchange")
        binary_hash = self.exec("app", "sha256sum /workspace/build/it-mq-producer").split()[0]
        source_hash = self.exec("app", "sha256sum /workspace/AIApps/ChatServer/src/AIUtil/MQManager.cpp").split()[0]
        require(all(re.fullmatch(r"[0-9a-f]{64}", value) is not None for value in (binary_hash, source_hash)),
                "Producer binary/source checksum unavailable")
        self.check("real_MQManager_publication_consumer_mysql_exact_deduplicated", lambda: {
            "message_id": exchange["message_id"], "publications_requested": 3,
            "broker_queue_publish_delta": queue_after["message_stats"]["publish"] - publish_before,
            "rows": len(rows), "sha256": digest(rows), "binary_sha256": binary_hash,
            "production_source": "AIApps/ChatServer/src/AIUtil/MQManager.cpp",
            "production_source_sha256": source_hash,
            "provenance": "real_MQManager_to_real_broker_to_application_consumer_to_real_mysql",
            "internal_publisher_confirm_callback_directly_observed": False,
        })

    def producer_publications(self, before):
        queue = self.queue()
        return queue if queue.get("message_stats", {}).get("publish", 0) - before >= 3 else None

    def histories(self):
        snapshot = self.snapshot()
        for key, account in self.accounts().items():
            self.login(account)
            own = {}
            for row in snapshot:
                if row["id"] == account["id"]:
                    own.setdefault(row["session_id"], []).append({"is_user": bool(row["is_user"]), "content": row["content"], "ts": row["ts"]})
            value, _ = self.api("GET", "/chat/sessions", account=account)
            sids = [x["sessionId"] for x in value["sessions"]]
            require(set(sids) == set(own) and len(sids) == len(set(sids)), "Session ownership/list differs from real persisted state")
            for sid, expected in own.items():
                value, _ = self.api("POST", "/chat/history", {"sessionId": sid}, account)
                require(value.get("history") == expected, "Restored history lost role/content/timestamp metadata")
            self.check("http_history_matches_mysql_" + key, lambda key=key, own=own: {"account": key, "sessions": len(own), "rows": sum(map(len, own.values())), "sha256": digest(own)})
        a, b = self.accounts()["a"], self.accounts()["b"]
        a_sessions = [row["session_id"] for row in snapshot if row["id"] == a["id"]]
        require(bool(a_sessions), "Persistence stage has no user A session")
        before = digest(self.snapshot())
        for path in ("/chat/history", "/chat/send"):
            self.api("POST", path, {"sessionId": a_sessions[0], "question": "must reject cross-user session", "modelType": "5"}, b, expected=400)
        self.api("POST", "/chat/history", {"sessionId": secrets.token_hex(16)}, a, expected=400)
        require(digest(self.snapshot()) == before, "Rejected history/send request changed durable data")
        self.check("cross_user_and_unknown_sessions_rejected_without_db_change", lambda: {"requests": 3, "database_sha256": before})

    def restart(self):
        require(bool(self.state["exchanges"]), "Run persistence stage first")
        before = self.snapshot()
        started = self.inspect("app")["State"]["StartedAt"]
        self.lifecycle("restart", "app")
        self.wait_http()
        require(self.inspect("app")["State"]["StartedAt"] != started, "App restart was not observed")
        require(self.snapshot() == before, "App restart changed persisted data")
        self.check("app_restart_preserves_mysql_rows", lambda: {"rows": len(before), "sha256_before": digest(before), "sha256_after": digest(self.snapshot())})
        self.histories()

    def db_restart(self):
        before = self.snapshot()
        self.lifecycle("restart", "mysql")
        self.wait(lambda: self.scalar("SELECT 1") == 1, "MySQL restart readiness", 120)
        require(self.snapshot() == before, "MySQL restart lost or changed persisted records")
        # Relogin before app restart tests the live pool's connection recovery.
        self.wait(lambda: self.try_login(self.accounts()["a"]), "live app database connection recovery", 120)
        self.check("mysql_restart_preserves_records_and_live_login_recovers", lambda: {"rows": len(before), "sha256_before": digest(before), "sha256_after": digest(self.snapshot())})
        self.lifecycle("restart", "app")
        self.wait_http()
        self.histories()

    def try_login(self, account):
        try:
            self.login(account)
            return True
        except (AssertionError, OSError, http.client.HTTPException):
            return False

    def broker_restart(self):
        self.accounts()
        exchange = self.new_exchange("a", "durable-before-broker-restart")
        self.add_exchange(exchange)
        app_stopped = False
        try:
            self.lifecycle("stop", "app")
            app_stopped = True
            self.publish(exchange)
            before = self.wait(lambda: self.peek(), "queued persistent message")
            require(any(json.loads(x["payload"]).get("message_id") == exchange["message_id"] and x.get("properties", {}).get("delivery_mode") == 2 for x in before), "Message was not queued with delivery_mode=2")
            self.lifecycle("restart", "rabbitmq")
            self.wait(lambda: self.queue().get("durable") is True, "durable queue after broker restart", 120)
            after = self.wait(lambda: self.peek(), "durable message after broker restart", 120)
            require(any(json.loads(x["payload"]).get("message_id") == exchange["message_id"] and x.get("properties", {}).get("delivery_mode") == 2 for x in after), "Persistent message missing after broker restart")
            self.check("broker_restart_preserves_persistent_queued_message", lambda: {"message_id": exchange["message_id"], "queue_durable": True, "delivery_mode": 2, "provenance": "real_broker_management_publication"})
        finally:
            if app_stopped:
                self.lifecycle("start", "app")
                self.wait_http()
        self.wait_exchange(exchange)
        self.publish(exchange)
        self.wait_queue_idle()
        require(self.exchange_rows(exchange) == self.expected_rows(exchange), "Durable duplicate changed exchange")
        self.check("durable_redelivery_is_idempotent", lambda: {"message_id": exchange["message_id"], "rows": 2, "sha256": digest(self.exchange_rows(exchange))})
        started = self.inspect("app")["State"]["StartedAt"]
        self.lifecycle("restart", "rabbitmq")
        self.wait(lambda: self.queue().get("consumers", 0) >= 1, "running app consumer broker reconnection", 150)
        require(self.inspect("app")["State"]["StartedAt"] == started, "App restarted during consumer reconnection test")
        reconnected = self.new_exchange("b", "after-live-broker-reconnect")
        self.add_exchange(reconnected)
        self.publish(reconnected)
        self.wait_exchange(reconnected)
        self.wait_queue_idle()
        self.check("live_consumer_reconnects_after_broker_restart", lambda: {"app_container_start_unchanged": True, "message_id": reconnected["message_id"], "rows": 2, "sha256": digest(self.exchange_rows(reconnected))})
        # Restore newly consumed messages into the HTTP history map.
        self.lifecycle("restart", "app")
        self.wait_http()
        self.histories()

    def gemma(self):
        if not self.args.gemma:
            raise Unavailable("Real Gemma generation not enabled; require real loaded weights and --gemma")
        account = self.accounts()["a"]
        self.login(account)
        before = self.snapshot()
        status, _, body = self.request("POST", "/chat/send-new-session", {
            "question": "Reply with one short greeting.", "modelType": "5", "stream": True,
        }, account["cookie"], timeout=self.args.model_timeout)
        require(status == 200, "Gemma SSE request failed before streaming")
        events = []
        for line in body.decode().splitlines():
            if line.startswith("data: "):
                events.append(line[6:])
        parsed = [json.loads(x) for x in events if x != "[DONE]"]
        errors = [x["error"] for x in parsed if "error" in x]
        if errors and any("model not loaded" in str(x).lower() for x in errors):
            require(self.snapshot() == before, "Unavailable model changed persisted history")
            raise Unavailable("Real Gemma model not loaded; actual SSE returned error and no rows were persisted")
        require(not errors, "Gemma SSE returned a generation error")
        require(events.count("[DONE]") == 1, "Gemma stream did not terminate exactly once")
        sid = next((x["sessionId"] for x in parsed if "sessionId" in x), None)
        answer = "".join(x.get("choices", [{}])[0].get("delta", {}).get("content", "") for x in parsed if "choices" in x)
        require(bool(answer.strip()) and sid, "Real Gemma returned no answer/session")
        rows = self.wait(lambda: self.rows_for_session(account["id"], sid), "real model chat persistence")
        require(len(rows) == 2 and rows[0]["content"] == "Reply with one short greeting." and rows[1]["content"] == answer and [r["is_user"] for r in rows] == [1, 0] and rows[1]["ts"] == rows[0]["ts"] + 1, "Real model answer does not match persisted exchange")
        self.check("real_gemma_sse_and_application_producer_persistence", lambda: {"session_id": sid, "answer_bytes": len(answer.encode()), "rows": 2, "sha256": digest(rows), "provenance": "actual_local_gemma_generation"})
        self.lifecycle("restart", "app")
        self.wait_http()
        self.histories()

    def rows_for_session(self, user_id, sid):
        rows = self.db("SELECT JSON_OBJECT('message_id',message_id,'is_user',is_user,'content',content,'ts',ts) FROM chat_message WHERE id=" + str(user_id) + " AND session_id=" + sql_text(sid) + " ORDER BY ts")
        return rows if len(rows) == 2 else None

    def run_stage(self, stage):
        self.checks = []
        start = time.monotonic()
        receipt = {"stage": stage, "run_id": self.state["run_id"], "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
        try:
            if stage != "gemma" or self.args.gemma:
                self.guard_target()
            {"prepare": self.prepare, "http": self.http, "auth": self.auth, "producer": self.producer,
             "persistence": self.persistence, "restart": self.restart,
             "db-restart": self.db_restart, "broker": self.broker_restart,
             "gemma": self.gemma}[stage]()
            receipt["status"] = "passed"
        except Unavailable as error:
            receipt.update(status="unavailable", reason=str(error))
        except Exception as error:
            receipt.update(status="failed", reason=type(error).__name__ + ": " + str(error))
        receipt.update(duration_seconds=round(time.monotonic() - start, 3), checks=self.checks,
                       check_counts={"passed": len(self.checks), "failed": int(receipt["status"] == "failed")})
        self.receipts.append(receipt)
        self.save()
        output = Path(self.args.evidence_dir) / (self.state["run_id"] + "-" + stage + "-" + str(time.time_ns()) + ".json")
        write_private(output, receipt)
        print(json.dumps({**receipt, "receipt": str(output)}, ensure_ascii=False), flush=True)
        return receipt["status"]

    def guard_target(self):
        data = self.inspect("app")
        bindings = data.get("HostConfig", {}).get("PortBindings") or {}
        expected_port = str(self.url.port or 80)
        require(any(binding.get("HostPort") == expected_port and binding.get("HostIp") in ("127.0.0.1", "::1")
                    for values in bindings.values() for binding in values),
                "BASE_URL does not match the labelled integration app's loopback port binding")
        app_environment = dict(item.split("=", 1) for item in data.get("Config", {}).get("Env", []) if "=" in item)
        require(app_environment.get("DB_NAME") == self.args.db_name,
                "Integration app DB_NAME differs from IT_DB_NAME")


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--stage", default="prepare,http,auth,persistence,restart,db-restart,broker,gemma",
                        help="Comma-separated stages; lifecycle stages restart isolated containers")
    result.add_argument("--probe", action="store_true", help="Only check HTTP readiness; never mutate containers/data")
    result.add_argument("--gemma", action="store_true", help="Attempt real local model generation; unavailable weights never pass")
    result.add_argument("--base-url", default=os.environ.get("BASE_URL", "http://127.0.0.1:18117"))
    result.add_argument("--docker", default=os.environ.get("IT_DOCKER", "docker"))
    result.add_argument("--docker-context", default=os.environ.get("IT_DOCKER_CONTEXT", "colima-mycppaiservice"))
    result.add_argument("--mysql-container", default=os.environ.get("IT_MYSQL_CONTAINER", "mycpp-it-mysql"))
    result.add_argument("--rabbit-container", default=os.environ.get("IT_RABBIT_CONTAINER", "mycpp-it-rabbitmq"))
    result.add_argument("--app-container", default=os.environ.get("IT_APP_CONTAINER", "mycpp-it-app"))
    result.add_argument("--control-container", default=os.environ.get("IT_CONTROL_CONTAINER", "mycpp-it-control"))
    result.add_argument("--db-name", default=os.environ.get("IT_DB_NAME", "mycpp_it"))
    result.add_argument("--state", default="/private/tmp/mycpp-integration/harness-state.json")
    result.add_argument("--evidence-dir", default="/private/tmp/mycpp-integration/evidence")
    result.add_argument("--timeout", type=float, default=15)
    result.add_argument("--restart-timeout", type=float, default=300)
    result.add_argument("--model-timeout", type=float, default=300)
    result.add_argument("--producer-seconds", type=float, default=12,
                        help="Real MQManager event-loop duration for optional producer stage (1-60 seconds)")
    return result


def main():
    args = parser().parse_args()
    runner = Runner(args)
    if args.probe:
        try:
            status, _, _ = runner.request("GET", "/__integration_ready__")
            ready = status == 404
            result = {"stage": "probe", "status": "passed" if ready else "unavailable", "http_status": status}
        except (OSError, http.client.HTTPException) as error:
            reason = "loopback network permission denied" if isinstance(error, PermissionError) else "service not ready"
            result = {"stage": "probe", "status": "unavailable", "reason": type(error).__name__ + ": " + reason}
        output = Path(args.evidence_dir) / (runner.state["run_id"] + "-probe-" + str(time.time_ns()) + ".json")
        write_private(output, result)
        print(json.dumps({**result, "receipt": str(output)}))
        return 0 if result["status"] == "passed" else 2
    stages = args.stage.split(",")
    require(all(x in STAGES for x in stages), "Unknown stage; choose from " + ",".join(STAGES))
    for stage in stages:
        if runner.run_stage(stage) == "failed":
            break
    counts = {status: sum(x["status"] == status for x in runner.receipts) for status in ("passed", "failed", "unavailable")}
    print(json.dumps({"run_id": runner.state["run_id"], "stage_counts": counts,
                      "not_run": [x for x in stages if x not in [r["stage"] for r in runner.receipts]],
                      "state": str(runner.state_path), "evidence_dir": args.evidence_dir,
                      "boundary": "Management-injected messages prove consumer behavior. Optional producer exercises real MQManager delivery and DB dedup but does not directly observe internal confirm callbacks. Real generation requires gemma stage."}))
    return 1 if counts["failed"] else 2 if counts["unavailable"] else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, ValueError) as error:
        print(json.dumps({"status": "failed", "reason": str(error)}))
        sys.exit(1)
