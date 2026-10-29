#!/usr/bin/env bash
set -euo pipefail

suite_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd -- "$suite_dir/../../.." && pwd)"
mode="${1:-normal}"
test_case="${2:-all}"
case "$mode" in normal|tsan|asan) ;; *) echo "Unknown build mode: $mode" >&2; exit 2 ;; esac
case "$test_case" in all|http|storage|data|expiry|manager|manager-pointer) ;;
    *) echo "Unknown test case: $test_case" >&2; exit 2 ;; esac
output_dir="${P1_TEST_OUTPUT:-${TMPDIR:-/tmp}/mycpp-p1-http-session-$mode}"
mkdir -p "$output_dir"
compiler="${CXX:-clang++}"
flags=(-std=c++17 -pthread -g -O1 -Wall -Wextra -Werror -include cassert
       -I"$suite_dir/stubs" -I"$repo_dir/HttpServer/include")
if [[ "$mode" == tsan ]]; then flags+=(-fsanitize=thread); fi
if [[ "$mode" == asan ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi

"$compiler" "${flags[@]}" "$suite_dir/http_request_test.cpp" \
    "$repo_dir/HttpServer/src/http/HttpRequest.cpp" \
    "$repo_dir/HttpServer/src/http/HttpContext.cpp" -o "$output_dir/http_request_test"
"$compiler" "${flags[@]}" "$suite_dir/session_thread_test.cpp" \
    "$repo_dir/HttpServer/src/session/Session.cpp" \
    "$repo_dir/HttpServer/src/session/SessionStorage.cpp" \
    "$repo_dir/HttpServer/src/session/SessionManager.cpp" \
    "$repo_dir/HttpServer/src/http/HttpRequest.cpp" \
    "$repo_dir/HttpServer/src/http/HttpResponse.cpp" -o "$output_dir/session_thread_test"

export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1 exitcode=66 symbolize=0}"
if [[ "$test_case" == all || "$test_case" == http ]]; then "$output_dir/http_request_test"; fi
if [[ "$test_case" == all ]]; then
    "$output_dir/session_thread_test" all
elif [[ "$test_case" != http ]]; then
    "$output_dir/session_thread_test" "$test_case"
fi
