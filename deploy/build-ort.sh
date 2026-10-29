#!/bin/sh
# Rebuild the pinned ORT CPU runtime without losing ARM64 cpuinfo ownership.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work=${ORT_WORK_DIR:-/tmp/mycpp-ort}
cache=${ORT_DOWNLOAD_CACHE:-$work/downloads}
prefix=${ORT_INSTALL_PREFIX:-/usr/local}
jobs=${BUILD_JOBS:-2}
case "$jobs" in ''|*[!0-9]*|0) echo 'BUILD_JOBS must be a positive integer' >&2; exit 2 ;; esac
case "$(uname -m)" in
    aarch64|arm64) cmake_arch=aarch64; cmake_sha=7909cc2128ce9442c63ce674a0bfb0e4f4ce04cef667d887e15ad5670d594ba7 ;;
    x86_64) cmake_arch=x86_64; cmake_sha=931e3c0d546ee03ca72bb147ccd9b49e3b6252f765f66bf21b9d165519940458 ;;
    *) echo 'Unsupported ORT build architecture' >&2; exit 2 ;;
esac
mkdir -p "$work" "$cache" "$prefix"

fetch() {
    url=$1 file=$2 checksum=$3
    if ! printf '%s  %s\n' "$checksum" "$file" | sha256sum -c - >/dev/null 2>&1; then
        curl -fL --retry 5 --retry-all-errors --connect-timeout 30 \
            --speed-time 60 --speed-limit 1024 --max-time 1800 "$url" -o "$file.part"
        printf '%s  %s\n' "$checksum" "$file.part" | sha256sum -c -
        mv "$file.part" "$file"
    fi
}

fetch https://github.com/pytorch/cpuinfo/archive/959002f82d7962a473d8bf301845f2af720e0aa4.zip \
    "$cache/cpuinfo.zip" a0f53ccfb477c57753c595df02bf79ed67bf092fd9a5c61ec5b8992b81bc1e65
fetch "https://github.com/Kitware/CMake/releases/download/v3.28.6/cmake-3.28.6-linux-$cmake_arch.tar.gz" \
    "$cache/cmake-3.28.6-linux-$cmake_arch.tar.gz" "$cmake_sha"

cmake_home="$work/cmake-3.28.6-linux-$cmake_arch"
if [ ! -x "$cmake_home/bin/cmake" ]; then tar -xzf "$cache/cmake-3.28.6-linux-$cmake_arch.tar.gz" -C "$work"; fi
cmake="$cmake_home/bin/cmake"
source_dir=${ORT_SOURCE_DIR:-$work/onnxruntime-1.18.0}
cpuinfo_dir="$work/cpuinfo-959002f82d7962a473d8bf301845f2af720e0aa4"
if [ ! -d "$source_dir/.git" ]; then
    git -c http.version=HTTP/1.1 clone --filter=blob:none --no-checkout --depth 1 \
        --single-branch --branch v1.18.0 https://github.com/microsoft/onnxruntime.git "$source_dir"
fi
test "$(git -c safe.directory="$source_dir" -C "$source_dir" rev-parse HEAD)" = 45737400a2f3015c11f005ed7603611eaed306a6
# Fetch every CPU implementation but omit large test model fixtures.
git -c safe.directory="$source_dir" -C "$source_dir" sparse-checkout set --no-cone --stdin <<'PATTERNS'
/*
!/*/
/cmake/
/include/
/onnxruntime/
/tools/
!/onnxruntime/test/testdata/
PATTERNS
git -c safe.directory="$source_dir" -C "$source_dir" -c http.version=HTTP/1.1 checkout --detach HEAD
git -c safe.directory="$source_dir" -C "$source_dir" diff --quiet HEAD -- cmake include onnxruntime tools
if [ ! -f "$cpuinfo_dir/CMakeLists.txt" ]; then (cd "$work" && "$cmake" -E tar xf "$cache/cpuinfo.zip"); fi
test "$(cat "$source_dir/VERSION_NUMBER")" = 1.18.0
# Populate ORT's built-in mirror with retrying, hash-checked CPU dependencies.
# CMake still verifies each archive against its original deps.txt checksum.
while IFS=';' read -r dep url checksum; do
    case "$dep" in
        abseil_cpp|date|google_nsync|safeint|utf8_range|protobuf|json|mp11|re2|microsoft_gsl|flatbuffers|onnx) ;;
        *) continue ;;
    esac
    archive="$source_dir/mirror/${url#https://}"
    if ! printf '%s  %s\n' "$checksum" "$archive" | sha1sum -c - >/dev/null 2>&1; then
        mkdir -p "$(dirname -- "$archive")"
        curl -fL --retry 5 --retry-all-errors --connect-timeout 30 \
            --speed-time 60 --speed-limit 1024 --max-time 1800 "$url" -o "$archive.part"
        printf '%s  %s\n' "$checksum" "$archive.part" | sha1sum -c -
        mv "$archive.part" "$archive"
    fi
done < "$source_dir/cmake/deps.txt"
# GitLab regenerated Eigen's ZIP bytes; validate the same commit via Git instead.
eigen_dir=${ORT_EIGEN_SOURCE_DIR:-$work/eigen}
eigen_commit=e7248b26a1ed53fa030c5c459f7ea095dfd276ac
if [ ! -d "$eigen_dir/.git" ]; then
    git -c http.version=HTTP/1.1 clone --filter=blob:none --no-checkout --depth 1 \
        https://gitlab.com/libeigen/eigen.git "$eigen_dir"
fi
if [ "$(git -c safe.directory="$eigen_dir" -C "$eigen_dir" rev-parse HEAD)" != "$eigen_commit" ]; then
    git -c safe.directory="$eigen_dir" -C "$eigen_dir" -c http.version=HTTP/1.1 \
        fetch --depth 1 origin "$eigen_commit"
fi
git -c safe.directory="$eigen_dir" -C "$eigen_dir" sparse-checkout set --no-cone --stdin <<'PATTERNS'
/*
!/*/
/Eigen/
/unsupported/Eigen/
PATTERNS
git -c safe.directory="$eigen_dir" -C "$eigen_dir" -c http.version=HTTP/1.1 checkout --detach "$eigen_commit"
git -c safe.directory="$eigen_dir" -C "$eigen_dir" diff --quiet HEAD
test "$(git -c safe.directory="$eigen_dir" -C "$eigen_dir" rev-parse HEAD)" = "$eigen_commit"
if git -C "$cpuinfo_dir" apply --check "$script_dir/ort-cpuinfo-ownership.patch" >/dev/null 2>&1; then
    git -C "$cpuinfo_dir" apply "$script_dir/ort-cpuinfo-ownership.patch"
else
    # Accept only an already-applied exact patch, never silently skip a mismatch.
    git -C "$cpuinfo_dir" apply --reverse --check "$script_dir/ort-cpuinfo-ownership.patch"
fi

"$cmake" -S "$source_dir/cmake" -B "$work/build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_POLICY_DEFAULT_CMP0069=NEW \
    -Donnxruntime_BUILD_SHARED_LIB=ON -Donnxruntime_BUILD_UNIT_TESTS=OFF \
    -Donnxruntime_ENABLE_LTO=ON -Donnxruntime_ENABLE_PYTHON=OFF \
    -DFETCHCONTENT_SOURCE_DIR_PYTORCH_CPUINFO="$cpuinfo_dir" \
    -DFETCHCONTENT_SOURCE_DIR_PYTORCH_CLOG="$cpuinfo_dir" \
    -Donnxruntime_USE_PREINSTALLED_EIGEN=ON -Deigen_SOURCE_PATH="$eigen_dir" \
    -DCMAKE_SHARED_LINKER_FLAGS=-flto=1
"$cmake" --build "$work/build" --target onnxruntime --parallel "$jobs"
# Install only the runtime SDK; do not overwrite system protobuf/abseil.
mkdir -p "$prefix/lib" "$prefix/include"
cp -a "$work/build/"libonnxruntime.so* "$prefix/lib/"
chmod 755 "$prefix/lib/libonnxruntime.so.1.18.0"
cp "$source_dir/include/onnxruntime/core/session/"*.h "$prefix/include/"
cp "$source_dir/include/onnxruntime/core/providers/cpu/cpu_provider_factory.h" "$prefix/include/"
mkdir -p "$prefix/share/mycpp-ort"
cp "$source_dir/LICENSE" "$source_dir/ThirdPartyNotices.txt" "$script_dir/ort-cpuinfo-ownership.patch" "$prefix/share/mycpp-ort/"
printf '%s\n' 'ORT 1.18.0 / 45737400a2f3015c11f005ed7603611eaed306a6' \
    'cpuinfo 959002f82d7962a473d8bf301845f2af720e0aa4 + ARM64 ownership patch' \
    > "$prefix/share/mycpp-ort/build.txt"
