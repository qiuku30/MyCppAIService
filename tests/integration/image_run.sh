#!/usr/bin/env bash
set -euo pipefail

# Run inside the dependency image only after its build has completed.
image_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
image_source_root="$(cd -- "$image_script_dir/../.." && pwd)"
image_model_dir="${1:-/models}"
image_build_dir="${2:-/it-build}"
image_mode="${3:-contract}"
image_ort_root="${ONNXRUNTIME_ROOT:-/usr/local}"
image_cxx="${CXX:-c++}"
image_extra_cxx_args=()
if [[ -n "${IMAGE_TEST_CXXFLAGS:-}" ]]; then
  read -r -a image_extra_cxx_args <<< "$IMAGE_TEST_CXXFLAGS"
fi

if [[ "$image_mode" != contract && "$image_mode" != oracle && "$image_mode" != resource ]]; then
  printf 'Expected contract, oracle or resource mode\n' >&2
  exit 2
fi
if [[ ! -r "$image_model_dir/mobilenetv2-7.onnx" || ! -r "$image_model_dir/imagenet_classes.txt" ]]; then
  printf 'Real model and ImageNet labels are required; this is not a valid RED\n' >&2
  exit 2
fi
if ! printf '%s  %s\n' \
  c1c513582d56afceff8516c73804e484c81c6a830712ab6d682253f4a3cd042f "$image_model_dir/mobilenetv2-7.onnx" \
  acf75ef0abe89694b19056e0796401068b459c457baa30335f240c7692857355 "$image_model_dir/imagenet_classes.txt" \
  | sha256sum --check; then
  printf 'Official asset hashes differ; this is not a valid RED\n' >&2
  exit 2
fi
if ! image_opencv_flags="$(pkg-config --cflags --libs opencv4)"; then
  printf 'Real OpenCV development files are required; this is not a valid RED\n' >&2
  exit 2
fi
read -r -a image_opencv_args <<< "$image_opencv_flags"
mkdir -p -- "$image_build_dir"
if ! "$image_cxx" -std=c++17 -O2 -DORT_API_MANUAL_INIT "${image_extra_cxx_args[@]}" \
  -I"$image_source_root/AIApps/ChatServer/include" \
  -I"$image_source_root/AIApps/ChatServer/src" -I"$image_ort_root/include" \
  "$image_script_dir/image_contract.cpp" \
  "$image_source_root/AIApps/ChatServer/src/AIUtil/ImageRecognizer.cpp" \
  -L"$image_ort_root/lib" -Wl,-rpath,"$image_ort_root/lib" -lonnxruntime \
  "${image_opencv_args[@]}" -pthread -o "$image_build_dir/image_contract"; then
  printf 'Image contract compilation failed; this is not a valid RED\n' >&2
  exit 2
fi
exec "$image_build_dir/image_contract" "$image_model_dir/mobilenetv2-7.onnx" \
  "$image_model_dir/imagenet_classes.txt" "$image_build_dir" "$image_mode"
