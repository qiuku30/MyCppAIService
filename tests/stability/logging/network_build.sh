#!/usr/bin/env bash
set -euo pipefail
source_root=${1:?source root required}
test_root=${2:?current test checkout required}
output=${3:?output directory required}
driver=${4:-$test_root/tests/stability/logging/network_server.cpp}
mkdir -p "$output"
sources=()
for unit in http/HttpRequest.cpp http/HttpContext.cpp http/HttpResponse.cpp http/HttpServer.cpp \
  router/Router.cpp middleware/MiddlewareChain.cpp middleware/cors/CorsMiddleware.cpp \
  ssl/SslConfig.cpp ssl/SslConnection.cpp ssl/SslContext.cpp \
  session/Session.cpp session/SessionStorage.cpp session/SessionManager.cpp; do
  sources+=("$source_root/HttpServer/src/$unit")
done
"${CXX:-c++}" -std=c++17 -O1 -g -pthread -I"$source_root/HttpServer/include" \
  "$driver" "${sources[@]}" \
  -lmuduo_net -lmuduo_base -lssl -lcrypto -ldl -o "$output/network_server"
