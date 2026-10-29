#!/usr/bin/env bash
set -euo pipefail
suite_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd -- "$suite_dir/../../.." && pwd)"
mode="${1:-normal}"
selected="${2:-all}"
case "$mode" in normal|asan|tsan) ;; *) exit 2 ;; esac
output_dir="${SESSION_TEST_OUTPUT:-/private/tmp/mycpp-stability-20261030/session/$mode}"
mkdir -p "$output_dir"
flags=(-std=c++17 -pthread -g -O1 -Wall -Wextra -Werror -include cassert
       -I"$repo_dir/tests/p1/http_session/stubs" -I"$repo_dir/HttpServer/include")
if [[ "$mode" == asan ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
if [[ "$mode" == tsan ]]; then flags+=(-fsanitize=thread); fi
"${CXX:-clang++}" "${flags[@]}" "$suite_dir/lifecycle_test.cpp" \
    "$repo_dir/HttpServer/src/session/Session.cpp" \
    "$repo_dir/HttpServer/src/session/SessionStorage.cpp" \
    "$repo_dir/HttpServer/src/session/SessionManager.cpp" \
    "$repo_dir/HttpServer/src/http/HttpRequest.cpp" \
    "$repo_dir/HttpServer/src/http/HttpResponse.cpp" -o "$output_dir/lifecycle_test"
export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1 exitcode=66 symbolize=0}"
if "$output_dir/lifecycle_test" "$selected" 2>"$output_dir/stderr.log"; then status=0; else status=$?; fi
cat "$output_dir/stderr.log" >&2
if grep -Fq 'PRIVATE-CLEANUP-DETAIL-MUST-NOT-BE-LOGGED' "$output_dir/stderr.log"; then
    echo 'FAIL: cleanup diagnostic exposed backend exception detail' >&2
    exit 1
fi
exit "$status"
