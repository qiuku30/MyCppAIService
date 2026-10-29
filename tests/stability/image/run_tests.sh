#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
out="${1:-/out}"
mkdir -p "$out"
printf '%s  %s\n' c1c513582d56afceff8516c73804e484c81c6a830712ab6d682253f4a3cd042f /models/mobilenetv2-7.onnx acf75ef0abe89694b19056e0796401068b459c457baa30335f240c7692857355 /models/imagenet_classes.txt | sha256sum --check
c++ -std=c++17 -O1 -g -DIMAGE_BOUNDARY_FULL -DORT_API_MANUAL_INIT ${IMAGE_TEST_CXXFLAGS:-} -I"$root/AIApps/ChatServer/include" -I"$root/AIApps/ChatServer/src" -I/usr/local/include "$root/tests/stability/image/boundary_test.cpp" "$root/AIApps/ChatServer/src/AIUtil/ImageRecognizer.cpp" -L/usr/local/lib -Wl,-rpath,/usr/local/lib -lonnxruntime $(pkg-config --cflags --libs opencv4) -pthread -o "$out/boundary_test"
"$out/boundary_test" /models/mobilenetv2-7.onnx /models/imagenet_classes.txt "$out"
c++ -std=c++17 -O1 ${IMAGE_TEST_CXXFLAGS:-} -I"$root/AIApps/ChatServer/include" "$root/tests/stability/image/policy_test.cpp" -o "$out/policy_test"
"$out/policy_test" defaults
IMAGE_MAX_BGR_BYTES=11 "$out/policy_test" bgr
IMAGE_MAX_CONCURRENT_JOBS=1 "$out/policy_test" quota
for value in '' 0 -1 xyz 18446744073709551616; do
 IMAGE_MAX_ORIGINAL_PIXELS="$value" "$out/policy_test" invalid
done
IMAGE_MAX_ENCODED_BYTES=2147483648 "$out/policy_test" invalid
printf 'Policy contracts: 9/9 passed\n'
