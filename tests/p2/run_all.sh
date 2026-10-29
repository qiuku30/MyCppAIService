#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$task_root"
task_mode=${1:-normal}
case "$task_mode" in normal|asan|tsan) ;; *) echo 'Expected normal, asan, or tsan' >&2; exit 2 ;; esac
export PYTHONDONTWRITEBYTECODE=1
export MYCPP_TEST_DEPS=${MYCPP_TEST_DEPS:-${TMPDIR:-/tmp}/mycpp-p1-deps}
export JSON_INCLUDE=${JSON_INCLUDE:-$MYCPP_TEST_DEPS}
if [[ -z ${OPENSSL_PREFIX:-} ]]; then
    if [[ -d /opt/homebrew/opt/openssl@3 ]]; then export OPENSSL_PREFIX=/opt/homebrew/opt/openssl@3
    else export OPENSSL_PREFIX=/usr; fi
fi
task_http_mode=normal
if [[ "$task_mode" == asan ]]; then
    task_http_mode=asan
    export HISTORY_TEST_CXXFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
    export P2_TEST_CXXFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
elif [[ "$task_mode" == tsan ]]; then
    export HISTORY_TEST_CXXFLAGS='-fsanitize=thread -g'
    export P2_TEST_CXXFLAGS='-fsanitize=thread -g'
fi
bash tests/p1/run_all.sh "$task_mode"
bash tests/p2/http/run.sh "$task_http_mode" all
python3 tests/p2/history/run.py
python3 tests/p2/rag/run.py
python3 tests/p2/deploy/run.py
echo "PASS combined P1/P2 regression groups ($task_mode)"
