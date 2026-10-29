# Stability boundaries: approved discussion and execution

User approved the proposed order twice: logs -> login Session lifecycle -> image resource budget, with real RED/GREEN and regression validation. User explicitly selected JPEG/PNG uploads. No commit/push; preserve all prior P1/P2/image/ORT changes and database history.

## Global constraints

- Existing checkout `/Users/kudoshinichi/MyCppAIService` contains the previous authorized repairs. Work incrementally here; baseline230 files copied to `/private/tmp/mycpp-stability-20261030/baseline` before edits. No resets, commits or pushes.
- Split into three independent deliverables and review each. Do not implement a new inference thread pool, outbox, schema migration, model sharing, or changes to chat ownership/history in this batch.
- Linux VM project mount remains read-only. Only independent SDK/output directories may be writable. Never print private Docker environment, credentials or real cookie values.
- Real OpenCV4.5.4 / ORT1.18.0 ownership-fixed SDK is retained. LSan stays enabled; no suppressions.

## 1. Log privacy

Replace whole HTTP response and untrusted raw-path logging with bounded metadata. Generate a process-local request identifier, use a registered route template/category, status, elapsed processing time and serialized response size for ordinary responses. SSE header takeover gets a stream-open record, not a fabricated inference completion/duration. Headers, cookies, authorization, body, model output and raw exception payloads are excluded. Escape route labels so logs cannot inject lines. The wire response must remain byte/semantically equivalent. Audit existing voice response prints and middleware raw input/error logging.

Acceptance: actual HttpServer callbacks generate no unique secret markers from cookie/auth/body/path/query/exception, while metadata and ordinary/SSE/error/404 behavior remain testable. Native source tests plus real Linux log capture at integration time; fake logging adapter is only a capture boundary.

## 2. Login Session lifecycle

Retain request-time expiry, add storage-level expired sweep and a periodically scheduled, stoppable cleanup owned by SessionManager. Default idle lifetime stays3600 seconds; default capacity10000 and cleanup interval60 seconds. Permit explicit constructor policy overrides for deterministic resource tests; validate positive values. Capacity admits/reclaims expired entries atomically, never randomly evicts valid sessions, and refuses new sessions with an explicit HTTP503 response. Existing-session updates and parallel admission must respect the same cap. Do not mint/send a usable cookie on failed admission. Keep constructor/storage public usage backwards compatible for current callers and tests where possible.

Map ownership removal must not destroy Session objects held by active requests. Live refresh/load and cleanup are synchronized; no wall-clock jump dependency for idle expiry. Worker waits are interruptible and destructor joins promptly. Callback save releases the Session mutex before acquiring the storage mutex. Database/chat history and model caches have independent lifetimes and are unchanged.

Acceptance: stale entries not revisited by clients are reclaimed, active entries preserved, cap cannot be exceeded by concurrent creation, expired capacity is reusable, HTTP overload is clear, shutdown is prompt, and normal/ASan/TSan threaded regressions pass. Use weak_ptr observation and real public interfaces, not test-only production methods.

## 3. JPEG/PNG resource limits

Validate format and dimensions before decoding, reject corrupt/truncated/overflowing headers, then recheck actual decoded shape and existing resize limits. Enforce an encoded-byte cap, original pixel/BGR-buffer budget and process-wide concurrent image-work cap through RAII; all prediction entrypoints use the same policy, including file and Mat callers. Begin with conservative documented defaults of16MiB encoded,16Mi pixels,64MiB projected decoded BGR buffer, and two simultaneous image jobs. The BGR bound is a buffer bound, not an assertion about all codec allocator internals or total process RSS. Validate with real model inference and measure resource behavior in the4GiB Linux environment.

JPEG baseline/progressive and valid PNG color/depth forms must use checked integer arithmetic; EXIF orientation does not bypass pixel limits. Allow only JPEG/PNG as confirmed by the user. Reject excess concurrency promptly rather than block I/O threads. Preflight rejection precedes per-user model construction on HTTP uploads. Keep all original prediction/string APIs, confidence, preprocessing, ownership and resize-attack regression behavior. Model-instance pooling remains a later task.

Acceptance: positive PNG/JPEG models and HTTP score checks, tiny compressed large-header images rejected before decoder/model work, malformed dimensions/segments and mismatched shapes rejected, file/Mat entrypoints bounded, RAII quota released on every failure, quota shared across users/instances, actual concurrent/source sanitizer checks and full application integration regressions.
