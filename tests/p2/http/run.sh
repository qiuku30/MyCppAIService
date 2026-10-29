#!/usr/bin/env bash
set -euo pipefail
suite_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd -- "$suite_dir/../../.." && pwd)"
mode="${1:-normal}"
selected="${2:-all}"
case "$mode" in normal|asan) ;; *) echo "Unknown mode: $mode" >&2; exit 2 ;; esac
case "$selected" in all|parser|response|cors|server|lifetime|dispatch) ;; *) echo "Unknown case: $selected" >&2; exit 2 ;; esac
output_dir="${P2_HTTP_OUTPUT:-${TMPDIR:-/tmp}/mycpp-p2-http-$mode}"
mkdir -p "$output_dir"
openssl_prefix="${OPENSSL_PREFIX:-/opt/homebrew/opt/openssl@3}"
flags=(-std=c++17 -pthread -g -O1 -Wall -Wextra -Werror -Wno-unused-private-field -include cassert
       -I"$suite_dir/stubs" -I"$repo_dir/tests/p1/http_session/stubs"
       -I"$repo_dir/HttpServer/include" -I"$openssl_prefix/include")
if [[ "$mode" == asan ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
"${CXX:-clang++}" "${flags[@]}" "$suite_dir/http_test.cpp" "$suite_dir/ssl_link_stubs.cpp" \
    "$repo_dir/HttpServer/src/http/HttpRequest.cpp" "$repo_dir/HttpServer/src/http/HttpContext.cpp" \
    "$repo_dir/HttpServer/src/http/HttpResponse.cpp" "$repo_dir/HttpServer/src/http/HttpServer.cpp" \
    "$repo_dir/HttpServer/src/session/Session.cpp" "$repo_dir/HttpServer/src/session/SessionStorage.cpp" \
    "$repo_dir/HttpServer/src/session/SessionManager.cpp" \
    "$repo_dir/HttpServer/src/router/Router.cpp" "$repo_dir/HttpServer/src/middleware/MiddlewareChain.cpp" \
    "$repo_dir/HttpServer/src/middleware/cors/CorsMiddleware.cpp" "$repo_dir/HttpServer/src/ssl/SslConfig.cpp" \
    -o "$output_dir/http_test"
"$output_dir/http_test" "$selected"
