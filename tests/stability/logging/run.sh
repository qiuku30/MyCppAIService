#!/usr/bin/env bash
set -euo pipefail
suite_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd -- "$suite_dir/../../.." && pwd)"
mode="${1:-normal}"
case "$mode" in normal|asan) ;; *) exit 2 ;; esac
json_include="${JSON_INCLUDE:-${MYCPP_TEST_DEPS:-/tmp/mycpp-p1-deps}}"
if [[ ! -f "$json_include/nlohmann/json.hpp" ]]; then
  printf 'Missing nlohmann/json.hpp under %s. Set JSON_INCLUDE or MYCPP_TEST_DEPS to a directory containing it.\n' "$json_include" >&2
  exit 2
fi
output_dir="${LOGGING_OUTPUT:-/private/tmp/mycpp-stability-20261030/logging}/$mode"
mkdir -p "$output_dir"
flags=(-std=c++17 -pthread -g -O1 -Wall -Wextra -Werror -Wno-unused-private-field -include cassert
       -I"$suite_dir/stubs" -I"$repo_dir/tests/p2/http/stubs" -I"$repo_dir/tests/p1/http_session/stubs"
       -I"$repo_dir/HttpServer/include" -I"${OPENSSL_PREFIX:-/opt/homebrew/opt/openssl@3}/include")
if [[ "$mode" == asan ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
"${CXX:-clang++}" "${flags[@]}" "$suite_dir/logging_test.cpp" "$repo_dir/tests/p2/http/ssl_link_stubs.cpp" \
  "$repo_dir/HttpServer/src/http/HttpRequest.cpp" "$repo_dir/HttpServer/src/http/HttpContext.cpp" \
  "$repo_dir/HttpServer/src/http/HttpResponse.cpp" "$repo_dir/HttpServer/src/http/HttpServer.cpp" \
  "$repo_dir/HttpServer/src/session/Session.cpp" "$repo_dir/HttpServer/src/session/SessionStorage.cpp" \
  "$repo_dir/HttpServer/src/session/SessionManager.cpp" \
  "$repo_dir/HttpServer/src/router/Router.cpp" "$repo_dir/HttpServer/src/middleware/MiddlewareChain.cpp" \
  "$repo_dir/HttpServer/src/middleware/cors/CorsMiddleware.cpp" "$repo_dir/HttpServer/src/ssl/SslConfig.cpp" \
  -o "$output_dir/logging_test"
"${CXX:-clang++}" "${flags[@]}" -I"$json_include" -I"$repo_dir/AIApps/ChatServer/include" -I"$repo_dir/AIApps/ChatServer/src" \
  "$suite_dir/speech_logging_test.cpp" "$repo_dir/AIApps/ChatServer/src/AIUtil/AISpeechProcessor.cpp" -o "$output_dir/speech_logging_test"
set +e
"$output_dir/logging_test" > "$output_dir/result.log" 2>&1
http_status=$?
"$output_dir/speech_logging_test" > "$output_dir/speech-result.log" 2>&1
speech_status=$?
set -e
cat "$output_dir/result.log" "$output_dir/speech-result.log"
if [[ "$http_status" != 0 || "$speech_status" != 0 ]]; then exit 1; fi
