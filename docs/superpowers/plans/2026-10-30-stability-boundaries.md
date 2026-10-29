# Stability Boundaries Implementation Plan

> Execute one task at a time with subagent-driven-development; task-specific review and a final integrated review. No commits/pushes; user-approved direct continuation in the existing checkout takes precedence over skill commit examples.

**Goal:** close response-log privacy, login Session retention/capacity and JPEG/PNG decode resource boundaries without changing chat persistence or inference scheduling.

**Architecture:** three independent source-linked RED/GREEN deliverables, existing APIs retained, then real Linux build/integration on the fixed ORT SDK. Full contract: ../specs/2026-10-30-stability-boundaries-design.md.

**Tech Stack:** C++17, muduo, OpenCV4.5.4, ORT1.18.0 ownership-fixed runtime, native Clang sanitizers, Linux GCC11/Colima, real MySQL/RabbitMQ fixtures.

## Global constraints

- Baseline230 files: /private/tmp/mycpp-stability-20261030/baseline. Preserve pre-existing P1/P2/image/ORT changes and .serena; no resets/commits/pushes.
- No DB schema/history change, thread-pool rewrite, outbox or model-sharing implementation in this batch.
- User confirmed JPEG/PNG. Defaults: encoded16MiB, original16Mi pixels, projected BGR64MiB, global image concurrency2; default Session TTL3600s/cap10000/cleanup60s.
- Keep sourceRO/SDKRW VM mounts, original real model assets and all LSan checks enabled. Capture source and real service evidence separately. No credential/cookie dumps.

## Task 1: Log privacy

- [x] RED: actual HttpServer source dispatch with captured muduo sink leaks unique cookie/body/exception/path markers; existing wire replies remain verifiable.
- [x] GREEN: bounded metadata request_id/method/trusted route/status/elapsed/bytes, stream_open category; remove payload/path/exception logging in scoped middleware/speech code.
- [x] Regression: task normal/ASan plus existing P2 HTTP normal/ASan. Task contract and report are /private/tmp/mycpp-stability-20261030/task-1-{brief,report}.md.
- [x] Independent task review: spec and quality verdict, fix important issues before continuing.

## Task 2: Session lifecycle

- [x] Record a fresh task baseline after Task1. Source scope Session.h/.cpp, SessionStorage.h/.cpp, SessionManager.h/.cpp; only narrow HttpResponse overload support and existing test runner linking if required. New tests in tests/stability/session.
- [x] RED: public cleanExpiredSessions does not reclaim expired map-owned objects; capacity/cleanup/shutdown concurrency contracts use actual source, not duplicate storage.
- [x] GREEN: synchronized sweep/capacity; monotonic idle expiry; compatible manager policy overrides; stoppable/joined cleanup; HTTP503 without a new cookie when capacity is full.
- [x] Regression: new normal/ASan/TSan; existing threaded P1 Session/HTTP suites. Validate load/refresh/cleanup ordering and held shared_ptr survival.
- [x] Independent task review and fix important issues.

## Task 3: Image resources

- [x] Record task baseline after Task2. Source scope ImageRecognizer.h/.cpp, optional small image-budget helper, AIUploadSendHandler.cpp, narrow HttpResponse status support, deployment resource-limit documentation; new tests in tests/stability/image and integration driver for actual libraries.
- [x] RED: valid tiny compressed JPEG/PNG with oversized dimensions reaches decoder/allocation before the budget; exercise real production entrypoints with safe allocation observation, not deliberate host OOM. Missing model/library/setup is not a product RED.
- [x] GREEN: checked JPEG/PNG metadata and byte/pixel/BGR limits before decode/model work, actual decoded verification, existing resize bound, shared RAII image permits, all file/Mat/buffer APIs bounded. Clear excess/unsupported-format errors.
- [x] Regression: normal plus ASan/UBSan/LSan actual OpenCV/ORT, PNG/JPEG/progressive, malformed/overflow, failure quota release, cross-instance concurrency and actual HTTP checks. Record limits as buffer/concurrency bounds and measure, not an unproven total-RSS guarantee.
- [x] Independent task review and fix important issues.

## Final integration

- [x] Whole-batch review against the approved design and task baselines.
- [x] Combined P1/P2 normal and relevant sanitizer regressions; fresh Linux app build on ownership-fixed SDK; real HTTP/MySQL/RabbitMQ56 checks and image HTTP.
- [x] Live safe log capture, Session overload/recovery with an isolated policy if needed, image decode rejection and normal-inference checks. Preserve pre-test history snapshot and verify intentional fixture writes separately.
- [x] Validate unchanged source files/dependencies, diff and syntax checks, persist exact commands/exits/hashes and update implementation ledger/report. Leave code uncommitted for user review.
