#!/usr/bin/env python3
"""Syntax-check real integration sources with real JSON/curl/OpenSSL, offline service ABI fixtures."""
import os
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
BUILD = pathlib.Path(tempfile.mkdtemp(prefix="mycpp-integration-compile-"))
JSON = pathlib.Path(os.environ.get("JSON_INCLUDE", "/private/tmp/mycpp-p1-deps"))
OPENSSL = pathlib.Path(os.environ.get("OPENSSL_PREFIX", "/opt/homebrew/opt/openssl@3"))
if not (JSON / "nlohmann/json.hpp").is_file():
    raise SystemExit("Set JSON_INCLUDE to a directory containing the real nlohmann/json.hpp")
shutil.copy(ROOT / "tests/p1/db_auth/sql_fake.h", BUILD / "sql_fake.h")
for name in ("cppconn/connection.h", "cppconn/prepared_statement.h", "cppconn/resultset.h", "mysql_driver.h"):
    target = BUILD / name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text('#pragma once\n#include "sql_fake.h"\n')
(BUILD / "mysql").mkdir()
(BUILD / "mysql/mysql.h").write_text("#pragma once\n")
include = [BUILD, ROOT, ROOT / "AIApps/ChatServer/include", ROOT / "HttpServer/include",
           JSON, OPENSSL / "include", ROOT / "tests/p1/mq/stubs"]
command = [os.environ.get("CXX", "clang++"), "-std=c++17", "-pthread", "-Wall", "-Wextra", "-fsyntax-only"]
for path in include:
    command += ["-I", str(path)]
sources = [ROOT / "AIApps/ChatServer/src/AIUtil" / name for name in
           ("AIHelper.cpp", "AIStrategy.cpp", "AIConfig.cpp", "AIToolRegistry.cpp")]
probe = BUILD / "message_api.cpp"
probe.write_text('''#include "AIUtil/ChatMessage.h"
#include "AIUtil/SecureId.h"
#include "HttpServer/include/utils/MysqlUtil.h"
void verify_interface(const std::string& body) {
    auto item = ChatExchange::parse(body);
    item.message_id = secureId();
    http::MysqlUtil database;
    item.persist(database);
}
''')
sources.append(probe)
for source in sources:
    print("SYNTAX", source.name, flush=True)
    subprocess.run(command + [str(source)], cwd=ROOT, check=True)
print(f"PASS: {len(sources)} integration translation units; BUILD={BUILD}", flush=True)
