#!/usr/bin/env python3
"""Compile real pool source and auth method bodies against bounded offline drivers."""
import pathlib
import shutil
import subprocess
import tempfile
import sys
import os

ROOT = pathlib.Path(__file__).resolve().parents[3]
TEST = pathlib.Path(__file__).resolve().parent
BUILD = pathlib.Path(tempfile.mkdtemp(prefix="mycpp-db-auth-"))
REF = sys.argv[1] if len(sys.argv) > 1 else None
def source_text(path):
    if REF:
        return subprocess.check_output(["git", "show", f"{REF}:{path}"], cwd=ROOT, text=True)
    return (ROOT / path).read_text()

db = BUILD / "HttpServer/include/utils/db"
db.mkdir(parents=True)
(BUILD / "HttpServer/src/utils/db").mkdir(parents=True)
for name in ("DbConnectionPool.h", "DbException.h"):
    (db / name).write_text(source_text(f"HttpServer/include/utils/db/{name}"))
shutil.copy(TEST / "pool_fake.h", db / "DbConnection.h")
source = BUILD / "HttpServer/src/utils/db/DbConnectionPool.cpp"
source.write_text(source_text("HttpServer/src/utils/db/DbConnectionPool.cpp"))
logging = BUILD / "muduo/base/Logging.h"
logging.parent.mkdir(parents=True)
logging.write_text("#pragma once\nstruct QuietLog { template<class T> QuietLog& operator<<(const T&) { return *this; } };\n#define LOG_INFO QuietLog{}\n#define LOG_WARN QuietLog{}\n#define LOG_ERROR QuietLog{}\n")
cxx = ["clang++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror"]
if os.environ.get("SANITIZE"):
    cxx += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
legacy_pool = ["-DLEGACY_POOL_API"] if REF else []
subprocess.run(cxx + legacy_pool + ["-I", str(BUILD), str(TEST / "pool_test.cpp"), str(source), "-o", str(BUILD / "pool")], check=True)

# Import handling is replaced by the fixture; method definitions are copied byte-for-byte.
auth = BUILD / "auth_methods.cpp"
parts = ['#include "auth_fixture.h"\n']
password = ROOT / "HttpServer/src/utils/PasswordUtil.cpp"
if password.exists():
    parts.append('#include "HttpServer/include/utils/PasswordUtil.h"\n')
for name, method in (("ChatRegisterHandler", "insertUser"), ("ChatLoginHandler", "queryUserId")):
    text = source_text(f"AIApps/ChatServer/src/handlers/{name}.cpp")
    parts.append(text[text.index(f"int {name}::{method}"):])
auth.write_text("\n".join(parts))
crypto = pathlib.Path(os.environ.get("OPENSSL_PREFIX", "/opt/homebrew/opt/openssl@3"))
cmd = cxx + ["-I", str(ROOT), "-I", str(TEST), str(TEST / "auth_test.cpp"), str(auth)]
if password.exists():
    cmd += [str(password), "-I", str(crypto / "include"), "-L", str(crypto / "lib"), "-lcrypto"]
subprocess.run(cmd + ["-o", str(BUILD / "auth")], check=True)

# Compile the real DbConnection implementation against an offline connector ABI fixture.
connector = BUILD / "connector"
connector.mkdir()
shutil.copy(TEST / "sql_fake.h", connector / "sql_fake.h")
for path in ("cppconn/connection.h", "cppconn/prepared_statement.h", "cppconn/resultset.h", "mysql_driver.h"):
    target = connector / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text('#pragma once\n#include "sql_fake.h"\n')
(connector / "mysql").mkdir()
(connector / "mysql/mysql.h").write_text("#pragma once\n")
for name in ("DbConnection.h", "DbException.h"):
    target = connector / "HttpServer/include/utils/db" / name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(source_text(f"HttpServer/include/utils/db/{name}"))
connection_source = connector / "HttpServer/src/utils/db/DbConnection.cpp"
connection_source.parent.mkdir(parents=True)
connection_source.write_text(source_text("HttpServer/src/utils/db/DbConnection.cpp"))
legacy = ["-DLEGACY_DB_API"] if REF else []
subprocess.run(cxx + legacy + ["-I", str(connector), "-I", str(BUILD), str(TEST / "connection_test.cpp"), str(connection_source), "-o", str(BUILD / "connection")], check=True)
if not REF:
    for path in ("HttpServer/include/utils/MysqlUtil.h", "HttpServer/include/utils/db/DbConnectionPool.h", "HttpServer/src/utils/db/DbConnectionPool.cpp"):
        target = connector / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(source_text(path))
    subprocess.run(cxx + ["-I", str(connector), "-I", str(BUILD), str(TEST / "query_lease_test.cpp"), str(connection_source), str(connector / "HttpServer/src/utils/db/DbConnectionPool.cpp"), "-o", str(BUILD / "query_lease")], check=True)
failures = 0
cases = [("pool", ["exclusive"]), ("pool", ["recovery"]), ("pool", ["lifetime"]), ("auth", []), ("connection", [])]
if not REF:
    cases.append(("query_lease", []))
    cases += [("pool", ["shutdown"]), ("pool", ["checkout_shutdown"])]
for executable, args in cases:
    print("RUN", executable, *args, flush=True)
    try:
        result = subprocess.run([str(BUILD / executable), *args], timeout=8)
        failures += result.returncode != 0
    except subprocess.TimeoutExpired:
        print("FAIL: process failed to stop within 8 seconds", flush=True)
        failures += 1
print(f"BUILD={BUILD} FAILURES={failures}", flush=True)
sys.exit(bool(failures))
