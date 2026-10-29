# Image resource contract

Generate fixtures on the host (the pinned SDK has no Python):

```
python3 tests/stability/image/make_fixture.py OUTPUT_DIRECTORY
```

Inside `mycppaiservice-build:integration`, with this project read-only at
`/project`, official assets read-only at `/models`, and the fixture/output
folder writable at `/out`, run:

```
bash /project/tests/stability/image/run_tests.sh /out
```

Use explicit private Docker config/context, network none, and a real persistent
SDK directory as output bind source; host `/private/tmp` is not shared by Colima.
For sanitizers add `IMAGE_TEST_CXXFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'`
and `ASAN_OPTIONS=detect_leaks=1`. No suppressions are used. The test uses actual
OpenCV decoding and real ONNX inference; the Run observer forwards to the real
library except one intentional error to exercise permit release. The OpenCV
allocation observer exists only in tests and safely blocks allocations >64MiB.

Policy environment (set before process startup; read and strictly validated once
on first use): `IMAGE_MAX_ENCODED_BYTES=16777216`,
`IMAGE_MAX_ORIGINAL_PIXELS=16777216`, `IMAGE_MAX_BGR_BYTES=67108864`,
`IMAGE_MAX_CONCURRENT_JOBS=2`. Values must be positive decimal integers fitting
size_t; encoded bytes must also fit OpenCV's int representation. These limits
apply process-wide across recognizers; header-only HTTP preflight precedes model
lookup. Busy processing returns503, original decode/encoded excess413,
unsupported/corrupt images400; the existing extreme resized geometry remains400.

Only JPEG (baseline/progressive, including grayscale) and PNG are supported.
The checked parser bounds container/header traversal without decoding pixels.
OpenCV remains responsible for full codec validation. PNG original dimensions,
color/depth, IHDR checksum, chunk lengths and image termination are checked.
JPEG marker/segment/scan traversal validates frame dimensions before decoding;
EXIF orientation may transpose decoded dimensions and remains enabled.

The BGR limit bounds projected output-buffer bytes, not codec temporary memory,
model memory, preexisting caller allocations or process RSS. Mat entrypoints
bound subsequent processing; callers already own their Mat allocation. Encoded
buffer entrypoints likewise receive caller-owned bytes. File entrypoints acquire
the global permit before file reads/copies. HTTP has already received its request
body before image preflight, and model construction/cache lifetime is outside the
image-job permit. The existing 16Mi-pixel resized limit still applies. No timeout
or full process memory cap is claimed. Tests report observed Linux maxRSS for the
whole two-recognizer fixture, including models and sanitizer overhead.
