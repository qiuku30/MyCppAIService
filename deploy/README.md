# Linux build and deployment

The service currently compiles Gemma and image recognition into the executable.
Cloud-only use still needs their build dependencies. Missing model files are
handled by the application's startup checks; this does not remove compile-time
dependencies.

## Dependency versions and architecture

The authoritative dependency image is `Dockerfile.build` (Ubuntu 22.04).
It installs MySQL's JDBC Connector/C++, the MySQL C client, OpenCV, OpenSSL,
libcurl and nlohmann/json from that distribution, plus these pinned sources:

| Dependency | Revision / output |
| --- | --- |
| muduo | [v2.0.2 commit b55b0ac](https://github.com/chenshuo/muduo/tree/b55b0ac6a10d4ded1301c3172e779ec46ada2ab3); shared muduo_net/base |
| AMQP-CPP | [839f74f](https://github.com/CopernicaMarketingSoftware/AMQP-CPP/tree/839f74f2e7cc0f3f03458ebf943d23fc1f8c5462); Linux TCP transport enabled |
| ONNX Runtime | [1.18.0](https://github.com/microsoft/onnxruntime/tree/45737400a2f3015c11f005ed7603611eaed306a6); x64 official CPU archive; ARM64 full CPU source build with the cpuinfo ownership patch |
| Gemma | [qiuku30/gemma.cpp 6abb6b1](https://github.com/qiuku30/gemma.cpp/tree/6abb6b1f5545d7d0059f2ed31afb095d655c9b73); external static build |

On ARM64, the image increases muduo v2.0.2's numeric formatting reserve from
32 to 48 bytes. Its existing static assertion requires more than 43 bytes for
Linux ARM64's binary128 `long double`; the size assertion remains enabled.

ARM64 ORT is rebuilt using `build-ort.sh` and
`ort-cpuinfo-ownership.patch`. The official 1.18.0 ARM64 binary loses three
cpuinfo cache owners under LTO and reports a verified 48-byte initialization
leak on the two-vCPU test VM. Only cpuinfo's IPO is disabled; CPU detection,
`-O3`, default CPU operators and LTO for ORT itself remain enabled. The helper
pins CMake 3.28.6, checks downloaded hashes, verifies source commits and retains
retryable dependency/build caches. Python is confined to the builder stage.
See [the leak regression](../tests/integration/ort_leak/README.md) for exact
reproduction and verification commands. The first ARM64 dependency build now
compiles ORT and needs more time/disk than extracting its prebuilt archive.

Gemma must be built for the **same Linux architecture, toolchain and ABI** as
this image. A macOS build cannot be mounted into a Linux container as a library.
Do not mix Gemma headers and archives from different revisions. Gemma's pinned
CMake file also pins Highway, SentencePiece and nlohmann/json. The commands below
keep optional OneDNN support off and use SentencePiece's internal absl/protobuf;
enabling external providers would require their extra link dependencies.
The application's Linux link hides symbols from the static SentencePiece
archive. Its bundled protobuf-lite has a different ABI from OpenCV DNN's system
protobuf; exporting both caused a verified crash during shared-library startup.

These source/version pins do not freeze Ubuntu package repositories or image
tags to digests. They are reproducible instructions for dependency selection,
not a claim of bit-for-bit reproducible image output.

## Prepare the dependency image and Gemma

From the MyCppAIService repository root, build the dependency image:

```sh
docker build -t mycppaiservice-build:local -f deploy/Dockerfile.build deploy
```

Use Docker with BuildKit support (the default with current Docker/Buildx).
APT retries transient downloads and retains its archive cache across failed
builds. ARM64 builds can select an Ubuntu Ports mirror while retaining APT's
normal repository signature checks, for example:

```sh
docker build --build-arg UBUNTU_PORTS_MIRROR=http://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports \
  -t mycppaiservice-build:local -f deploy/Dockerfile.build deploy
```

Use a separate writable directory for the Gemma checkout and build. The paths
below are examples to replace with real absolute paths on your Linux host:

The included one-line Gemma patch makes its unsupported-query assertion depend
on the template's query type. GCC11 otherwise rejects `static_assert(false)`
even in a discarded C++17 branch; unsupported types still fail compilation.

```sh
export GEMMA_SOURCE_HOST_DIR=/absolute/path/to/gemma.cpp
export GEMMA_BUILD_HOST_DIR=/absolute/path/to/gemma.cpp/build
export GEMMA_MODELS_HOST_DIR=/absolute/path/to/gemma-models
git clone https://github.com/qiuku30/gemma.cpp.git "$GEMMA_SOURCE_HOST_DIR"
git -C "$GEMMA_SOURCE_HOST_DIR" checkout --detach 6abb6b1f5545d7d0059f2ed31afb095d655c9b73
git -C "$GEMMA_SOURCE_HOST_DIR" apply "$PWD/deploy/gemma-cxx17-compat.patch"
mkdir -p "$GEMMA_BUILD_HOST_DIR" "$GEMMA_MODELS_HOST_DIR"
docker run --rm --entrypoint sh \
  -v "$GEMMA_SOURCE_HOST_DIR:/opt/gemma-src" \
  -v "$GEMMA_BUILD_HOST_DIR:/opt/gemma" \
  mycppaiservice-build:local -c '
    set -eu
    cmake -S /opt/gemma-src -B /opt/gemma \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
      -DGEMMA_ENABLE_TESTS=OFF -DGEMMA_ONEDNN_BRGEMM=OFF \
      -DSPM_ENABLE_SHARED=OFF -DSPM_ABSL_PROVIDER=internal \
      -DSPM_PROTOBUF_PROVIDER=internal
    cmake --build /opt/gemma --target libgemma --parallel 2
  '
```

The build uses `/opt/gemma-src` and `/opt/gemma`, matching their later container
mount points. If that build directory was configured under different paths or
for another architecture, create a new build directory.

The expected outputs from this exact Gemma tree are:

- `build/libgemma.a`
- `build/_deps/highway-build/libhwy.a`
- `build/_deps/highway-build/libhwy_contrib.a`
- `build/_deps/sentencepiece-build/src/libsentencepiece.a`
- Highway headers in `build/_deps/highway-src`
- SentencePiece headers in `build/_deps/sentencepiece-src/src`

Place your compatible licensed model and tokenizer files into
`GEMMA_MODELS_HOST_DIR`. Defaults are `2.0-2b-it-sfp.sbs` and `tokenizer.spm`;
change the filename variables in `.env` if yours differ. Acquiring or testing
model weights is separate from building the service.

## Compose configuration

```sh
cd deploy
cp .env.example .env
# Edit .env: passwords, API credentials, absolute Gemma host paths, model filenames.
docker compose config --quiet
docker compose build ai-service
docker compose up -d
docker compose logs -f ai-service
```

`GEMMA_*_HOST_DIR` values are bind-mount sources only. The process always reads
its model under `/opt/gemma-models`, source under `/opt/gemma-src`, and archives
under `/opt/gemma`. `GEMMA_MODEL_FILE` and `GEMMA_TOKENIZER_FILE` must be filenames,
not absolute host paths. The source checkout is mounted read-only and the
application build uses its own `app_build` volume, so host build caches are not
reused across platforms. The entrypoint configures the build directory before
compiling and then runs the binary from that directory.

`KNOWLEDGE_BASE_ID` is retained as the environment name for compatibility; its
value is the Bailian application **AppId**, not a retrieval dataset ID.

The example uses an application RabbitMQ user. RabbitMQ restricts its
[`guest` user to loopback connections](https://www.rabbitmq.com/docs/access-control#loopback-users);
the service connects from another container. Existing RabbitMQ volumes keep
their existing users/passwords, so use an already provisioned user or explicitly
provision the intended account before switching the service configuration.

The old `Dockerfile` is a compatibility wrapper around the same local image,
with the same mounts and entrypoint. Build the dependency image first; the
wrapper has no separate library-copy/install recipe:

```sh
docker build -t mycppaiservice-build:local -f deploy/Dockerfile.build deploy
docker build --pull=false -t mycppaiservice-wrapper:local -f deploy/Dockerfile deploy
```

## Optional image recognition assets

The clean dependency image contains the ONNX Runtime library. To use image
recognition, provide the MobileNet V2 ONNX model and its matching ImageNet
labels file, then include the separate `docker-compose.image.yml` override:

```sh
cd deploy
export IMAGE_MODEL_HOST_FILE=/absolute/path/to/mobilenetv2-7.onnx
export IMAGE_LABELS_HOST_FILE=/absolute/path/to/imagenet_classes.txt
test -f "$IMAGE_MODEL_HOST_FILE" && test -r "$IMAGE_MODEL_HOST_FILE"
test -f "$IMAGE_LABELS_HOST_FILE" && test -r "$IMAGE_LABELS_HOST_FILE"
docker compose -f docker-compose.yml -f docker-compose.image.yml config --quiet
docker compose -f docker-compose.yml -f docker-compose.image.yml up -d --build ai-service
```

Alternatively, uncomment and set those two variables in `.env`. The override
adds two read-only file mounts at the recognizer's existing container paths:
`/root/models/mobilenetv2/mobilenetv2-7.onnx` and
`/root/imagenet_classes.txt`. `create_host_path: false` makes a missing host file
a mount error instead of silently creating a directory. Supply regular readable
files; the model/labels contents and inference compatibility require a real
image-recognition smoke test.

This override is opt-in. The base Compose configuration does not require these
auxiliary assets for cloud chat or Gemma startup. No model download is performed
by either recipe.

### Image formats and resource limits

Image recognition accepts JPEG (baseline/progressive, including grayscale) and
PNG. Other formats are rejected; there is no automatic format fallback.
Authenticated uploads receive header and original-dimension checks before
per-user model creation, followed by actual decoded-shape validation. The
existing resized-image limit of 16 Mi pixels remains in effect.

The base Compose environment exposes these optional policy settings, also shown
in `.env.example`:

| Variable | Default | Meaning |
| --- | ---: | --- |
| `IMAGE_MAX_ENCODED_BYTES` | `16777216` | 16 MiB encoded image payload |
| `IMAGE_MAX_ORIGINAL_PIXELS` | `16777216` | 16 Mi pixels before resizing |
| `IMAGE_MAX_BGR_BYTES` | `67108864` | 64 MiB projected decoded BGR buffer |
| `IMAGE_MAX_CONCURRENT_JOBS` | `2` | Shared process-wide image processing permits |

Set values before starting the process. The image policy reads and validates
these variables once on first use; changing an environment file does not update
a running process. Recreate the application container to apply changed values.
Values must be positive decimal integers fitting `size_t`; encoded bytes must
also fit `INT_MAX`. Invalid process values fail closed. Compose uses the defaults
when a variable is unset or empty, so remove an override to restore its default.
The policy does not enable the optional model mounts or download assets.

Unsupported, corrupt or invalid images return HTTP400. Exceeding the encoded or
original decoded-image budget returns HTTP413 (Payload Too Large). Exhausting the
shared image-job quota returns HTTP503 (Service Unavailable); retry later. The
existing extreme-aspect-ratio preprocessing rejection remains HTTP400.

The BGR byte setting bounds a projected output buffer, not codec internals,
intermediate buffers, model caches or process RSS. Mat/buffer APIs receive
caller-owned allocations; the policy bounds subsequent processing. File APIs
acquire the quota before reading image bytes. HTTP request/base64 storage and
per-user model creation occur outside the inference permit. These settings do
not impose a hard process-memory limit or an inference timeout. See the
[real-library boundary tests](../tests/stability/image/README.md) for scope and
verification details.

## Database initialization and existing volumes

`MYSQL_DATABASE` selects both the MySQL initialization database and the
application's `DB_NAME`. On a **new empty** MySQL data directory, the
[official MySQL entrypoint](https://hub.docker.com/_/mysql) creates this database
and runs `init.sql` in it. That script creates `users` with a 255-character
password field for hashes and `chat_message` with nullable unique `message_id`
for deduplicated writes.

Existing MySQL data volumes are not reinitialized. The initialization mount is
not a migration and does not alter existing table definitions. Back up the
selected database and follow [password migration](auth-password-migration.md)
and the explicit scripts under `migrations/` for the existing schema. Keep
`MYSQL_DATABASE` set to the existing application's actual database; changing
that variable does not move its records. Do not delete data volumes to apply
schema changes.

## Native CMake build

Install the same dependencies for the native Linux target, then configure:

```sh
cmake -S . -B build \
  -DGEMMA_SOURCE_DIR=/absolute/path/to/gemma.cpp \
  -DGEMMA_BUILD_DIR=/absolute/path/to/gemma.cpp/build \
  -DCMAKE_PREFIX_PATH=/absolute/path/to/dependency-prefix
cmake --build build --parallel 2
export DB_HOST=127.0.0.1 DB_PORT=3306 DB_USER=your_user DB_NAME=your_database
export DB_PASSWORD=your_password RABBITMQ_USER=your_rabbit_user RABBITMQ_PASS=your_rabbit_password
export GEMMA_MODEL_PATH=/absolute/path/to/model.sbs
export GEMMA_TOKENIZER_PATH=/absolute/path/to/tokenizer.spm
bash run.sh -p 8117
```

Use `ONNXRUNTIME_ROOT` for a separate ONNX installation. CMake also accepts
explicit `*_INCLUDE_DIR` and `*_LIBRARY` cache paths shown in
`CMakeLists.txt`; missing dependencies fail during configuration with the
variable name. `-DGEMMA_...=...` cache arguments override environment defaults.
When changing source/build roots in an existing CMake build, use a new build
directory or clear the corresponding cached dependency paths.

`run.sh` uses the repository's build directory (or `BUILD_DIR`), preserves
caller-provided credentials/database settings, and forwards all server
arguments. Without arguments it selects port 8117, or `PORT` if supplied.

## Verification boundary

Run `python3 tests/p2/deploy/run.py` for offline contracts and CMake generation
with empty dependency placeholders. The placeholders are deliberately never
compiled or linked. These tests validate discovery, cache precedence, paths,
launcher behavior and schema wiring. They do not validate a Linux image build,
ABI compatibility, real MySQL/RabbitMQ startup, model inference, or deployment.
Perform the actual Docker build, database migration/restart and model smoke
tests on the target Linux host before treating this as a deployable artifact.
