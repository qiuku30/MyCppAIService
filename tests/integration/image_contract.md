# Real OpenCV / ONNX image contract

This contract uses the actual production `ImageRecognizer.cpp`, real OpenCV,
ONNX Runtime 1.18 CPU, and the official ONNX Model Zoo MobileNetV2 opset 7 model.
The C API observation table forwards every `Run` to the real runtime with the
original arguments. It records the actual production input and output tensors;
it does not supply model outputs. The three public prediction entry points are
exercised using a PNG, encoded PNG buffer, and OpenCV Mat.
The same real-model contract also requires construction to reject 999- and
1001-line label files against the model's 1000 output classes. No prediction is
attempted with those invalid files: the baseline's unchecked label-count bound
could otherwise read beyond its real output tensor.

The independent reference follows the official model preprocessing policy:
RGB, resize the shorter side to 256 while preserving aspect ratio, center crop
224x224, divide by 255, normalize by mean `[0.485, 0.456, 0.406]` and standard
deviation `[0.229, 0.224, 0.225]`, then NCHW. The fixtures have integral scaled
dimensions and use OpenCV bilinear interpolation; the test does not assert
Pillow/OpenCV interpolation bit identity. It asserts the actual submitted tensor
against that policy, and obtains expected classes and confidence from a separate
real ONNX session. Confidence is stable softmax of all 1000 logits.

Official sources pinned at `onnx/models` commit
`c5cae0f1942c8d7727372c7b1f6b485aa39e4421`:

- [Model](https://media.githubusercontent.com/media/onnx/models/c5cae0f1942c8d7727372c7b1f6b485aa39e4421/validated/vision/classification/mobilenet/model/mobilenetv2-7.onnx):
  14,246,826 bytes, SHA256 `c1c513582d56afceff8516c73804e484c81c6a830712ab6d682253f4a3cd042f`.
- [Labels](https://raw.githubusercontent.com/onnx/models/c5cae0f1942c8d7727372c7b1f6b485aa39e4421/validated/vision/classification/synset.txt):
  31,675 bytes, 1000 lines, SHA256 `acf75ef0abe89694b19056e0796401068b459c457baa30335f240c7692857355`.
- [Official preprocessing](https://github.com/onnx/models/blob/c5cae0f1942c8d7727372c7b1f6b485aa39e4421/validated/vision/classification/imagenet_preprocess.py).
- [ORT manual API initialization](https://github.com/microsoft/onnxruntime/blob/v1.18.0/include/onnxruntime/core/session/onnxruntime_cxx_api.h#L89).

The host assets are `/private/tmp/mycpp-integration/models/mobilenetv2-7.onnx`
and `/private/tmp/mycpp-integration/models/imagenet_classes.txt`. Both are public
image classification assets. This test never downloads or substitutes Gemma.

Run only after the parent verifies the dedicated dependency image is available.
Use the dedicated private Docker config and context explicitly on every call:

```bash
docker --config /private/tmp/mycpp-integration/docker-config \
  --context colima-mycppaiservice run --rm --pull never --network none \
  --cpus 1 --memory 1g --workdir /it-build \
  -v /Users/kudoshinichi/MyCppAIService:/workspace:ro \
  -v /private/tmp/mycpp-integration/models:/models:ro \
  -v /private/tmp/mycpp-integration/image-check:/it-build \
  mycppaiservice-build:integration \
  bash /workspace/tests/integration/image_run.sh /models /it-build contract
```

Use `oracle` as the last argument to emit independent reference PNGs and
`oracle.json` without evaluating production preprocessing. The contract run also
emits those files even when its completed production inferences differ. The
compiler runs as one process; inference uses one intra-op thread. No services
are started by this command.

For the real HTTP check, the parent may add `image_mounts.yml` as the final
Compose override before starting the app. The override deliberately supplies the
existing hardcoded production paths so a missing model cannot mask the semantic
RED. With the app and real dependencies already healthy:

```bash
python3 /Users/kudoshinichi/MyCppAIService/tests/integration/image_http_contract.py \
  --base-url http://127.0.0.1:18117 \
  --oracle /private/tmp/mycpp-integration/image-check/oracle.json \
  --receipt /private/tmp/mycpp-integration/image-check/http-red.json
```

The HTTP contract creates one ephemeral account in the dedicated integration DB,
logs in with a real session, and checks real `/upload/send` class names and
confidence for the reference PNGs. Credentials/cookies are neither printed nor
saved. Only explicit loopback HTTP addresses are allowed.

Exit `1` means observed product behavior differs after real inference completed:
valid RED. Exit `2` means missing assets, compilation/loading/HTTP setup failure,
or another incomplete observation: unavailable evidence, not a valid RED.
Exit `0` proves only these image contracts. It does not establish broad accuracy,
Gemma generation, concurrent-user memory limits, package/release, or acceptance.

Minimal proposed production change after valid RED and parent approval:

1. Add `ImageRecognizer::Prediction { std::string label; double confidence; }`
   and `PredictFromBufferWithConfidence` / `PredictFromMatWithConfidence`.
2. Keep the three existing string-returning APIs as delegating wrappers.
3. Perform the model's RGB/resize/crop/mean/std/NCHW preprocessing before real Run;
   reject mismatched model/label class counts at construction, check actual output
   bounds before reading logits, and calculate numerically stable softmax top-1
   confidence from those logits.
4. Let `AIUploadSendHandler` return this result's label and confidence, preserving
   its existing JSON fields and route.

No production implementation belongs before the relevant valid RED is recorded.

## Approved first GREEN and API boundary

The original eight-assertion RED is preserved in
`/private/tmp/mycpp-integration/image-check/cpp-red.json`; API presence is not
counted as an old product defect. The extended contract calls the new Mat and
Buffer confidence APIs and compares their actual returned label and score with
independent real-model softmax output, recording the forwarded real Run count.

- [x] Record the original real-model tensor/class/label-count RED.
- [x] Modify only `ImageRecognizer.h` and `ImageRecognizer.cpp` for the first GREEN.
- [x] Run the same real dependencies and official assets, plus actual score comparisons.
- [x] Save source hashes, exit status, and GREEN receipt; keep the upload handler
  for its separately recorded real HTTP confidence RED and later approval.

The supported model is one float32 input `[1,3,224,224]` and one float32 output
`[1,1000]`, with exactly 1000 labels. Dynamic dimensions, other tensor element
or container types, and additional inputs or outputs are rejected. Runtime output
shape/type/count must still match before any logits are read.

Mat input must be a nonempty, two-dimensional `CV_8UC3` BGR image. Grayscale,
BGRA, floating-point, other depths/channel counts, and higher-dimensional Mats
are rejected; this API does not silently convert them. File and encoded-buffer
entry points use OpenCV color decoding to produce an 8-bit BGR image, rejecting
failed decoding. Dimensions too large for OpenCV's integer resize dimensions are
rejected. Resized preprocessing output is capped at 16*1024*1024 pixels before
color conversion or resize. This does not introduce an alternate preprocessing
mode or an image-decoder allocation policy.

`Prediction.confidence` is the top class's probability from stable softmax over
all 1000 finite model logits, returned as `double` in `[0,1]`. It is not a
calibrated accuracy estimate. Non-finite logits or inconsistent model/label/output
contracts throw `std::runtime_error`; the existing string-returning APIs delegate
and keep their return type and entry points. No file confidence overload is added.

## Extreme-aspect-ratio resource guard

The separately approved `resource` mode loads the same verified real model and
calls the real production Mat path with a `1x16384 CV_8UC3` image (49,152 bytes).
Its short-side resize requests `256x4194304x3`, or 3,221,225,472 bytes. A scoped
default `cv::MatAllocator` observer forwards allocations up to 64 MiB unchanged to
the real OpenCV allocator, records and rejects larger requests, then restores the
original default. It supplies no fake image or inference output. The test expects
the product to reject before any oversized allocation request and before any Run
attempt; a rejection by the observer is a product contract failure.

[OpenCV 4.5.4 source](https://github.com/opencv/opencv/blob/4.5.4/modules/core/src/matrix.cpp#L696-L712)
shows that Mat::create rethrows when its selected allocator is already the
default fallback allocator. Installing the observer as the default therefore
prevents standard-allocator fallback from bypassing this safety limit.

Run with `resource` as the final argument to `image_run.sh`, preserving the same
private config/context, no-network, 1 CPU/1 GiB, and `/it-build` workdir limits.
This mode writes `resource.json`; its separate valid RED is preserved in
`/private/tmp/mycpp-integration/image-check/resource-red-run.json`. The observed
RED was exit 1, with one blocked 3 GiB request, largest forwarded allocation
49,152 bytes, and zero Run attempts. No OOM was used as evidence.

- [x] Safely reproduce the expansion through real OpenCV before any production edit.
- [x] Add the approved 16*1024*1024 resized-pixel budget guard before color
  conversion and resize, preserving normal image behavior.
- [x] Re-run resource and normal contracts: both exit 0; the resource case requests
  no intermediate OpenCV allocation and makes zero Run attempts.
- [x] After the separate three-failure real HTTP RED, change the handler to return
  the actual prediction label and score.
- [ ] Parent runs the extended real HTTP GREEN after its app rebuild.

This resource contract bounds the aspect-ratio expansion in preprocessing. Image
decoder allocation policy and broad upload quotas remain outside this change.

The extended HTTP harness preserves the three positive class/score comparisons
and generates a real 16384x1, 8-bit RGB PNG with stdlib struct/zlib for the
negative case. It requires HTTP 400, JSON status `error`, and a message containing
`resource budget`. HTTPError responses are recorded as observed behavior, so a
wrong status or wrong error message fails the product contract instead of being
reported as an operational exception. The local loopback client explicitly
disables environment proxies. Credentials and cookies remain unrecorded.
