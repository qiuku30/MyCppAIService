#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$task_root"
task_mode=${1:-normal}
case "$task_mode" in normal|asan|tsan) ;; *) echo 'Expected normal, asan, or tsan' >&2; exit 2 ;; esac
export PYTHONDONTWRITEBYTECODE=1
export MYCPP_TEST_DEPS=${MYCPP_TEST_DEPS:-${TMPDIR:-/tmp}/mycpp-p1-deps}
export JSON_INCLUDE=${JSON_INCLUDE:-$MYCPP_TEST_DEPS}
if [[ ! -f "$MYCPP_TEST_DEPS/nlohmann/json.hpp" ]]; then
    echo 'Set MYCPP_TEST_DEPS to a directory containing nlohmann/json.hpp (see tests/p1/README.md).' >&2
    exit 2
fi
if [[ -z ${OPENSSL_PREFIX:-} ]]; then
    if [[ -d /opt/homebrew/opt/openssl@3 ]]; then export OPENSSL_PREFIX=/opt/homebrew/opt/openssl@3
    else export OPENSSL_PREFIX=/usr; fi
fi
if [[ "$task_mode" == asan ]]; then
    export SANITIZE=1
    export GEMMA_TEST_CXXFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
    export MCP_TEST_CXXFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
elif [[ "$task_mode" == tsan ]]; then
    export GEMMA_TEST_CXXFLAGS='-fsanitize=thread -g'
    export MCP_TEST_CXXFLAGS='-fsanitize=thread -g'
fi
bash tests/p1/http_session/run_tests.sh "$task_mode" all
python3 tests/p1/db_auth/run.py
bash tests/p1/gemma/run.sh
python3 tests/p1/mq/run.py "$task_mode"
python3 tests/p1/mcp/run.py
python3 tests/p1/persistence/run.py
node tests/p1/frontend/stream_test.js
python3 tests/p1/integration_compile/run.py
echo "PASS P1 regression groups ($task_mode)"
