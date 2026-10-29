#!/bin/sh
set -eu

unavailable() {
    printf '{"driver":"real_MQManager","status":"unavailable","reason":"%s"}\n' "$1"
    exit 2
}

[ "$(uname -s)" = Linux ] || unavailable real_linux_environment_required
script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repo_dir="$(CDPATH= cd -- "$script_dir/../.." && pwd)"
build_dir="${IT_MQ_PRODUCER_BUILD_DIR:-/workspace/build}"
cxx="${CXX:-c++}"
mkdir -p "$build_dir" || unavailable driver_build_directory_unavailable
umask 077
build_log="$build_dir/it-mq-producer-build.log"
binary="$build_dir/it-mq-producer"

# Always compile current production sources against the real dependency image.
# No test include directories, generated stand-ins, or model sources are used.
if ! "$cxx" -std=c++17 -O1 -g -pthread \
    -I"$repo_dir/AIApps/ChatServer/include" \
    -I"$repo_dir/HttpServer/include" -I/usr/local/include \
    "$script_dir/mq_producer.cpp" \
    "$repo_dir/AIApps/ChatServer/src/AIUtil/MQManager.cpp" \
    -L/usr/local/lib -Wl,-rpath,/usr/local/lib \
    -lmuduo_net -lmuduo_base -lamqpcpp -lssl -lcrypto -ldl \
    -o "$binary" >"$build_log" 2>&1; then
    unavailable real_dependency_driver_compile_failed
fi

# Stdin remains the structured exchange; the shell/compiler never prints it.
# Normalize loader/driver failures to unavailable (2), never a product RED.
if "$binary" "$@"; then
    exit 0
fi
unavailable real_dependency_driver_execution_failed
