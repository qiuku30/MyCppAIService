#!/usr/bin/env python3
"""Compile complete production history functions against offline service boundaries."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import re
import json

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
SOURCE = Path(os.environ.get("MYCPP_TEST_SOURCE_ROOT", ROOT))
sys.path.insert(0, str(ROOT / "tests/p1/mcp"))
from common import function

DEPS = Path(os.environ.get("MYCPP_TEST_DEPS", "/private/tmp/mycpp-p1-deps"))
helper_source = (SOURCE / "AIApps/ChatServer/src/AIUtil/AIHelper.cpp").read_text()
helper_header = (SOURCE / "AIApps/ChatServer/include/AIUtil/AIHelper.h").read_text()
server_source = (SOURCE / "AIApps/ChatServer/src/ChatServer.cpp").read_text()
server_header = (SOURCE / "AIApps/ChatServer/include/ChatServer.h").read_text()
main_source = (SOURCE / "AIApps/ChatServer/src/main.cpp").read_text()
handler_source = (SOURCE / "AIApps/ChatServer/src/handlers/ChatHistoryHandler.cpp").read_text()
method_names = r"setStrategy|restoreMessage|chat|chatStream|request|GetMessages|GetHistorySnapshot|validateCanonicalHistory|commitExchange"
helper_methods = "\n\n".join(
    function(helper_source[match.start():], match.group(0))
    for match in re.finditer(r"^.+ AIHelper::(?:" + method_names + r")\(", helper_source, re.M)
)
replacements = {
    "// @HELPER_CLASS@": function(helper_header, "class AIHelper {") + ";",
    "// @HELPER_CTOR@": re.search(r"^AIHelper::AIHelper\(\) = default;", helper_source, re.M).group(0),
    "// @HELPER_METHODS@": helper_methods,
    "// @SERVER_CLASS@": function(server_header, "class ChatServer {") + ";",
    "// @SERVER_METHODS@": "\n\n".join(function(server_source, signature) for signature in (
        "void ChatServer::initChatMessage(", "void ChatServer::readDataFromMySQL(")),
    "// @HISTORY_HANDLER@": function(handler_source, "void ChatHistoryHandler::handle("),
    "// @STARTUP_METHODS@": function(main_source, "static " +
        ("bool" if "static bool waitForDependencies(" in main_source else "void") + " waitForDependencies(") +
        "\n\n" + function(main_source, "int main("),
    "// @SNAPSHOT_FEATURE@": "#define HISTORY_HAS_SNAPSHOT " + str(int("GetHistorySnapshot(" in helper_header)),
}

def compile_fixture(directory, name):
    text = (Path(__file__).parent / name).read_text()
    for marker, body in replacements.items():
        text = text.replace(marker, body)
    output = directory / Path(name).stem
    unit = output.with_suffix(".cpp")
    unit.write_text(text)
    command = [os.environ.get("CXX", "clang++"), "-std=c++17", "-O0", "-g", "-pthread",
               "-Wall", "-Wextra", "-I" + str(SOURCE), "-I" + str(DEPS),
               *shlex.split(os.environ.get("HISTORY_TEST_CXXFLAGS", "")), str(unit), "-o", str(output)]
    subprocess.run(command, check=True)
    return output

handler_prefix = (ROOT / "tests/p1/mcp/handlers_test.cpp").read_text().split("int main() {", 1)[0]
checked_index = '''
struct CheckedMutex {
    bool held = false;
    void lock() { if (held) throw std::runtime_error("duplicate mutex lock"); held = true; }
    bool try_lock() { if (held) return false; held = true; return true; }
    void unlock() { held = false; }
};
struct CheckedSessions : std::unordered_map<int, std::vector<std::string>> {
    CheckedMutex& mutex;
    explicit CheckedSessions(CheckedMutex& value) : mutex(value) {}
    std::vector<std::string>& operator[](int user) {
        if (!mutex.held) throw std::runtime_error("session index mutated without its shared mutex");
        return std::unordered_map<int, std::vector<std::string>>::operator[](user);
    }
};
'''
handler_prefix = handler_prefix.replace("struct Server {", checked_index + "\nstruct Server {")
handler_prefix = handler_prefix.replace("std::mutex mutexForSessionsId;", "CheckedMutex mutexForSessionsId;")
handler_prefix = handler_prefix.replace("std::unordered_map<int, std::vector<std::string>> sessionsIdsMap;", "CheckedSessions sessionsIdsMap{mutexForSessionsId};")
for name in ("ChatSendHandler", "ChatCreateAndSendHandler"):
    handler_source = (SOURCE / ("AIApps/ChatServer/src/handlers/" + name + ".cpp")).read_text()
    handler_prefix = handler_prefix.replace("// @" + name.upper() + "@", function(handler_source, "void " + name + "::handle("))
replacements["// @HANDLER_PREFIX@"] = handler_prefix

failed = 0
with tempfile.TemporaryDirectory(prefix="mycpp-p2-history-") as temporary:
    directory = Path(temporary)
    binary = compile_fixture(directory, "history_test.cpp")
    if subprocess.run([str(binary)]).returncode:
        failed += 1
    persisted = directory / "offline-db-snapshot.json"
    persisted.write_text(json.dumps({
        "rows": [[42, "restart", 1, "persistent question", 1000],
                 [42, "restart", 0, "persistent answer", 1001]],
        "expected": [{"content": "persistent question", "ts": 1000, "is_user": True},
                     {"content": "persistent answer", "ts": 1001, "is_user": False}],
    }))
    for restart in (1, 2):
        result = subprocess.run([str(binary), str(persisted)], capture_output=True, text=True)
        if result.returncode or "RESTART_REPLAY_OK" not in result.stdout:
            failed += 1
            print(f"FAIL offline_process_restart_replay_{restart}: exit={result.returncode}\n{result.stdout}{result.stderr}", flush=True)
        else:
            print(f"PASS offline_process_restart_replay_{restart}", flush=True)
    index_binary = compile_fixture(directory, "index_lock_test.cpp")
    if subprocess.run([str(index_binary)]).returncode:
        failed += 1
    binary = compile_fixture(directory, "startup_test.cpp")
    for scenario in ("success", "transient", "permanent", "constructor"):
        result = subprocess.run([str(binary)], env={**os.environ, "HISTORY_STARTUP_SCENARIO": scenario},
                                capture_output=True, text=True)
        if scenario == "constructor":
            valid = result.returncode == 1 and "ChatServer initialization/startup failed" in result.stderr and "SERVING" not in result.stdout
        elif scenario == "permanent":
            valid = result.returncode == 1 and "SERVING" not in result.stdout and "ATTEMPTS=30" in result.stdout
        else:
            valid = result.returncode == 0 and "SERVING restored history" in result.stdout
        if not valid:
            failed += 1
            print(f"FAIL startup_{scenario}: exit={result.returncode}\n{result.stdout}{result.stderr}", flush=True)
        else:
            print(f"PASS startup_{scenario}", flush=True)
if failed:
    raise SystemExit(f"History regression failed in {failed} fixture/scenario runs")
