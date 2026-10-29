# ORT ARM64 initialization ownership regression

This test uses actual ONNX Runtime and the exact upstream cpuinfo source. It
does not stub initialization, suppress allocations, or disable leak detection.

On the dedicated two-vCPU Linux ARM64 VM, the official ORT 1.18.0 library leaks
three 16-byte allocations even when the program only creates and destroys an
`Ort::Env`. The allocations correspond to cpuinfo's uarch table and Linux CPU
to processor/core maps. Upstream publishes these arrays into process-lifetime
globals and deliberately leaves `cpuinfo_deinitialize()` empty. Whole-program
optimization can remove unused private ownership stores while retaining the
`calloc` calls. The ARM64 release disassembly and a controlled source build
both demonstrate this loss of ownership.

The repair disables IPO only for the two cpuinfo targets on Linux ARM64.
CPU detection, `-O3`, all default ORT CPU operators, and the remaining ORT LTO
optimization remain enabled. These caches remain intentionally owned for the
process lifetime; this is not a claim that each `Ort::Env` frees global caches.

## Real ORT check

Inside Linux with GCC's ASan/UBSan runtime installed:

```sh
sh tests/integration/ort_leak/run_env.sh /usr/local /tmp/ort-env-check
```

This builds the probe against the chosen SDK and runs five independent
processes, each with ten `Ort::Env` lifetimes. Every process must exit zero with
`detect_leaks=1`. The original library exits one on the first process. The log
also records the actual loaded SDK's library SHA256 and runtime version.

## Controlled cpuinfo reproduction

Use cpuinfo commit `959002f82d7962a473d8bf301845f2af720e0aa4`, the revision in
ORT 1.18.0's `cmake/deps.txt`. Set `CPUINFO_SOURCE_DIR` to an unmodified copy:

```sh
cmake -S tests/integration/ort_leak -B /tmp/cpuinfo-red \
  -DCMAKE_BUILD_TYPE=Release -DPROBE_CPUINFO_LTO=ON \
  -DCPUINFO_SOURCE_DIR=/absolute/path/to/cpuinfo
cmake --build /tmp/cpuinfo-red --parallel 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  /tmp/cpuinfo-red/cpuinfo_ownership_probe
```

The shared dependency has a private export boundary, matching ORT's
encapsulation. Its public processor/core/uarch data is actually consumed.
CMP0069 is explicitly NEW so older dependency CMake policy cannot silently
ignore the requested IPO. Check the generated `flags.make` for `-flto`.

For the control, repeat in a separate build directory with
`PROBE_CPUINFO_LTO=OFF`. Then apply `deploy/ort-cpuinfo-ownership.patch` to a
separate source copy and repeat with the caller's IPO setting still ON. Both
controls must retain valid CPU metadata and exit zero. The patched cpuinfo
target must have `-fno-lto` and no generated `-flto` flag.

## Runtime rebuild

`deploy/build-ort.sh` pins ORT commit
`45737400a2f3015c11f005ed7603611eaed306a6` and CMake 3.28.6. It verifies the
cpuinfo/CMake downloads and ORT dependency mirror archives. Eigen is checked
out at ORT's original commit `e7248b26a1ed53fa030c5c459f7ea095dfd276ac` because
the current GitLab ZIP bytes no longer match the old recorded archive hash.
No archive checksum is bypassed. Source caches can be supplied using
`ORT_SOURCE_DIR`, `ORT_EIGEN_SOURCE_DIR` and `ORT_DOWNLOAD_CACHE`; both Git
checkouts must still have the exact pinned HEAD and unchanged tracked code.

The authoritative ARM64 `deploy/Dockerfile.build` calls the same helper in a
builder stage and copies only the resulting runtime SDK into its final image.
The existing x64 binary archive route is retained. ARM64 runtime checks do not
establish x64 results or cover uninstrumented third-party internal memory access.
