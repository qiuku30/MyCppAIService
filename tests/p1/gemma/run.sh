#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd "$(dirname "$0")/../../.." && pwd)
task_build=$(mktemp -d "${TMPDIR:-/tmp}/gemma-p1-build.XXXXXX")
trap 'rm -rf "$task_build"' EXIT
"${CXX:-c++}" -std=c++17 -pthread -Wall -Wextra -Werror ${GEMMA_TEST_CXXFLAGS:-} \
  -I"$task_root/tests/p1/gemma/fake" -I"$task_root/AIApps/ChatServer/include" \
  "$task_root/tests/p1/gemma/pool_test.cpp" \
  "$task_root/AIApps/ChatServer/src/AIUtil/GemmaInferencePool.cpp" -o "$task_build/pool_test"
task_fail=0
for task_case in policy files bounded budget past-budget model-cap lru history ttl callback-failure concurrent clear shutdown; do
  "$task_build/pool_test" "$task_case" || task_fail=1
done
exit "$task_fail"
