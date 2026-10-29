#!/bin/bash
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${BUILD_DIR:-${script_dir}/build}"
if [ ! -x "${build_dir}/http_server" ]; then
    printf 'Missing executable: %s/http_server. Build it with CMake first.\n' "$build_dir" >&2
    exit 1
fi
if [ "$#" -eq 0 ]; then
    set -- -p "${PORT:-8117}"
fi
cd -- "$build_dir"
exec ./http_server "$@"
