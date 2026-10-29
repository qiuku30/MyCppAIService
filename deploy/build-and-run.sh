#!/bin/sh
set -eu
repo_dir="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
build_dir="${BUILD_DIR:-${repo_dir}/build}"
cmake -S "$repo_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --parallel "${BUILD_JOBS:-2}"
if [ "$#" -eq 0 ]; then
    set -- -p "${PORT:-8117}"
fi
cd "$build_dir"
exec ./http_server "$@"
