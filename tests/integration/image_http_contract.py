#!/usr/bin/env python3
"""Compare real /upload/send responses with the independently inferred oracle.

Only a loopback integration app is accepted. An ephemeral test account is created;
its credentials and cookies are never printed or saved.
"""
import argparse
import base64
import http.cookiejar
import ipaddress
import json
import math
from pathlib import Path
import secrets
import struct
import sys
import zlib
import urllib.error
import urllib.parse
import urllib.request


def extreme_aspect_ratio_png():
    """A real 16384x1, 8-bit RGB PNG with one unfiltered scanline."""
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))

    header = struct.pack(">IIBBBBB", 16384, 1, 8, 2, 0, 0, 0)
    scanline = b"\x00" + bytes((241, 83, 17)) * 16384
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
            + chunk(b"IDAT", zlib.compress(scanline)) + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default="http://127.0.0.1:18117")
    parser.add_argument("--oracle", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    args = parser.parse_args()
    target = urllib.parse.urlsplit(args.base_url)
    try:
        loopback = target.hostname == "localhost" or ipaddress.ip_address(target.hostname).is_loopback
    except (ValueError, TypeError):
        loopback = False
    if (target.scheme != "http" or not loopback or not target.port or target.username
            or target.password or target.query or target.fragment or target.path not in ("", "/")):
        raise ValueError("An explicit loopback HTTP integration address and port are required")

    oracle = json.loads(args.oracle.read_text())
    cases = oracle["cases"]
    confidences = [float(item["confidence"]) for item in cases]
    if len(cases) < 2 or any(not math.isfinite(x) or not 0 < x <= 1 for x in confidences):
        raise ValueError("Oracle does not contain real finite softmax predictions")
    if len({round(x, 6) for x in confidences}) < 2:
        raise ValueError("Oracle needs distinct confidences to distinguish a constant response")

    cookies = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(
        urllib.request.ProxyHandler({}), urllib.request.HTTPCookieProcessor(cookies))

    def post(path, payload):
        request = urllib.request.Request(args.base_url.rstrip("/") + path,
            data=json.dumps(payload).encode(), method="POST",
            headers={"Content-Type": "application/json", "Connection": "close"})
        try:
            response = opener.open(request, timeout=30)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            return response.status, json.loads(response.read())

    account = {"username": "image_it_" + secrets.token_hex(10), "password": secrets.token_urlsafe(24)}
    register_status, registered = post("/register", account)
    if register_status not in (200, 201) or registered.get("status") != "success":
        raise RuntimeError("Image test registration failed")
    login_status, logged_in = post("/login", account)
    if login_status != 200 or logged_in.get("success") is not True or not list(cookies):
        raise RuntimeError("Image test login did not establish a real session")

    receipt = {"stage": "image_http", "status": "passed", "cases": [], "failures": 0}
    for expected in cases:
        filename = expected["filename"]
        if Path(filename).name != filename:
            raise ValueError("Oracle fixture filename must be local to its receipt directory")
        image = args.oracle.parent / filename
        status, response = post("/upload/send", {"filename": filename,
            "image": base64.b64encode(image.read_bytes()).decode("ascii")})
        confidence = response.get("confidence")
        valid_confidence = (isinstance(confidence, (int, float)) and not isinstance(confidence, bool)
                            and math.isfinite(confidence) and 0 <= confidence <= 1)
        entry = {"name": expected["name"], "http_status": status, "http_status_matches": status == 200,
                 "filename_matches": response.get("filename") == filename,
                 "success_matches": response.get("success") == "ok",
                 "class_matches": response.get("class_name") == expected["class_name"],
                 "expected_class_name": expected["class_name"], "actual_class_name": response.get("class_name"),
                 "expected_confidence": expected["confidence"], "actual_confidence": confidence,
                 "confidence_matches": valid_confidence and abs(confidence - expected["confidence"]) <= 5e-5}
        receipt["cases"].append(entry)
        receipt["failures"] += sum(not entry[key] for key in
            ("http_status_matches", "filename_matches", "success_matches", "class_matches", "confidence_matches"))

    extreme = extreme_aspect_ratio_png()
    status, response = post("/upload/send", {"filename": "extreme_aspect_ratio.png",
        "image": base64.b64encode(extreme).decode("ascii")})
    message = response.get("message")
    resource = {"name": "extreme_aspect_ratio", "source_shape": [1, 16384, 3],
                "png_bytes": len(extreme), "http_status": status,
                "http_status_matches": status == 400, "error_status_matches": response.get("status") == "error",
                "message": message, "resource_budget_matches": isinstance(message, str) and "resource budget" in message.lower()}
    receipt["resource_case"] = resource
    receipt["failures"] += sum(not resource[key] for key in
        ("http_status_matches", "error_status_matches", "resource_budget_matches"))
    if receipt["failures"]:
        receipt["status"] = "failed"
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    args.receipt.write_text(json.dumps(receipt, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({**receipt, "receipt": str(args.receipt)}, ensure_ascii=False))
    return 1 if receipt["failures"] else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        # Operational errors are not product RED evidence. No request bodies,
        # credentials, cookies, or full HTTP responses are included.
        print(json.dumps({"stage": "image_http", "status": "unavailable",
                          "reason": type(error).__name__ + ": " + str(error)[:300]}), file=sys.stderr)
        sys.exit(2)
