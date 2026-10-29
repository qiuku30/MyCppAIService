from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[3]
out = Path(tempfile.mkdtemp(prefix="mycpp-mq-"))
json_dir = Path(os.environ.get("MYCPP_TEST_DEPS", "/private/tmp/mycpp-p1-deps"))
mode = sys.argv[1] if len(sys.argv) > 1 else "normal"
if mode not in ("normal", "asan", "tsan"):
    raise SystemExit("expected normal, asan, or tsan")
flags = ["-std=c++17", "-g", "-O1", "-pthread", "-Wall", "-Wextra", "-Werror"]
if mode == "asan":
    flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
elif mode == "tsan":
    flags += ["-fsanitize=thread"]
environment = dict(os.environ)
environment.setdefault("TSAN_OPTIONS", "halt_on_error=1 exitcode=66 symbolize=0")
results = []
print("evidence:", out, flush=True)
for name in ("test", "extended_test"):
    command = [os.environ.get("CXX", "clang++"), *flags,
               "-I" + str(root / "tests/p1/mq/stubs"), "-I" + str(root),
               "-I" + str(root / "AIApps/ChatServer/include"), "-I" + str(json_dir),
               str(root / ("tests/p1/mq/" + name + ".cpp")),
               str(root / "AIApps/ChatServer/src/AIUtil/MQManager.cpp"),
               "-o", str(out / name)]
    subprocess.run(command, check=True)
    result = subprocess.run([str(out / name)], text=True, capture_output=True,
                            env=environment, timeout=60)
    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)
    (out / (name + ".log")).write_text(result.stdout + result.stderr)
    results.append(result.returncode)
raise SystemExit(1 if any(results) else 0)
