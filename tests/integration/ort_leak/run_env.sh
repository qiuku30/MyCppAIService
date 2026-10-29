#!/bin/sh
# Linux: test actual ORT lifetimes with leak checking enabled in fresh processes.
set -eu
ort=${1:?Usage: run_env.sh ORT_PREFIX OUTPUT_DIRECTORY}
out=${2:?Usage: run_env.sh ORT_PREFIX OUTPUT_DIRECTORY}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$out"
g++ -std=c++17 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
    -I"$ort/include" "$here/ort_env_probe.cpp" \
    -L"$ort/lib" -Wl,-rpath,"$ort/lib" -lonnxruntime \
    -o "$out/ort_env_probe"
sha256sum "$ort/lib/libonnxruntime.so.1.18.0"
for run in 1 2 3 4 5; do
    printf 'Independent process %s/5\n' "$run"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        LSAN_OPTIONS=exitcode=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        LD_LIBRARY_PATH="$ort/lib" "$out/ort_env_probe"
done
