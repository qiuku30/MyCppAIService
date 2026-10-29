# P2 Correctness Implementation Plan

Goal: repair all remaining reviewed P2 defects and verify P1 behavior remains intact.
Architecture: preserve HTTP/strategy/pool boundaries; tighten protocol parsing, transactional history restore, provider-specific streaming and deploy dependency discovery.

- [x] HTTP: source-linked RED for lower/mixed-case headers, invalid lengths/framing, valid CORS/404/500 response serialization; implement and run normal/sanitizer tests.
- [x] History: RED for startup restore enabled, exact content/metadata/ownership reconstruction and retry without duplication; stage and publish state only on success, test read errors.
- [x] RAG: RED for environment-name mismatch and actual provider event format; implement documented incremental adapter and completed/error states; test fragmented streams and prior P1 chat failures.
- [x] Deployment: RED for old AMQP dependency, hardcoded paths, missing selected-DB bootstrap and host/container path mismatch; implement targeted CMake/Docker/compose/scripts/doc fixes; test configuration and schema wiring.
- [x] Integration: independently review changed scopes, run all P1/P2 normal and applicable sanitizers, actual dependency syntax/config checks and a real build where this host supports it.
- [x] Delivery: map each reviewed P2 to fixed/retested evidence, document actual runtime limits and migration compatibility; leave changes reviewable in this workspace.

## Final verification (2026-10-29)

Combined source suites exited 0 on the final production code in all three modes:

- `MYCPP_TEST_DEPS=/private/tmp/mycpp-p1-deps bash tests/p2/run_all.sh normal` — `/private/tmp/mycpp-p2-all-normal.log`
- `MYCPP_TEST_DEPS=/private/tmp/mycpp-p1-deps bash tests/p2/run_all.sh asan` — `/private/tmp/mycpp-p2-all-asan.log`
- `MYCPP_TEST_DEPS=/private/tmp/mycpp-p1-deps bash tests/p2/run_all.sh tsan` — `/private/tmp/mycpp-p2-all-tsan.log`

Each entrypoint reruns all P1 groups. P2 evidence comprises 100 HTTP assertions, 25 history cases/scenarios, 26 RAG assertions and 12 deployment tests. These counts have different units and are not summed. ASan includes UBSan; TSan instrumentation covers threaded P1 slices, history and RAG, while HTTP/deployment run normally. Five real JSON/curl/OpenSSL integration translation units compile in every combined run.

HTTP source tests cover case-insensitive fields, strict bounded framing, default/CORS/error response serialization and EventLoop lifetime. An additional coalesced-input probe first demonstrated 19/34 dispatch failures, then terminal-input retention tests failed12/46; final100/100 normal and ASan/UBSan include close/error/SSE ownership across future callbacks and disposal of unusable terminal input. HEAD/TLS implementation was not changed or runtime-validated.

History restoration stages both maps, preserves explicit row roles/content/timestamps and publishes only after complete success. Unknown history reads no longer create caller-selected sessions; creation shares the session index lock. Independent fixture processes reconstruct the same disk-backed fake database, not a real MySQL restart. Invalid legacy role sequences remain readable but cannot continue model generation through parity-based APIs. Equal legacy timestamps have no fabricated chronology. Constructor DB-init failures exit clearly; only post-initialization history-query failures are retried.

RAG uses the documented application endpoint/header/incremental text/terminal format, canonical environment naming with legacy compatibility, strict event validation and curl handle/header-list ownership. Original14 behavior checks, later2 shape checks and6 controlled header-allocation checks were RED before their corresponding repairs. Real provider calls were not made. Provider contracts: https://help.aliyun.com/zh/model-studio/agent-and-workflow-application-api-reference and https://curl.se/libcurl/c/curl_slist_append.html .

Deployment removes user/architecture-specific dependency link paths, pins current AMQP-CPP Linux transport and muduo, unifies container model paths, mounts selected-DB initialization for new volumes and documents explicit existing-volume migration. An optional image asset override supplies the paths expected by the existing recognizer. Initial9 deployment tests and2 later asset checks were RED. Configuration fixtures use deliberately empty library placeholders and are never compiled or linked; passing them does not establish a deployable image.

Native configure is attempted in `/private/tmp/mycpp-p2-final-native-configure` and stops at the unavailable OpenCV package on this macOS host. Docker/Linux execution, real MySQL/RabbitMQ, model/provider inference, migration on user data, broker/disk durability and UI acceptance remain unverified. Existing P1 schema migration requirements are unchanged; no real database/schema was mutated here. The19 protected P1 DB/session/password/MQ/Gemma/schema files match the pre-P2 hashes; no pre-existing file was removed. `git diff --check` and shell syntax checks passed. No commit, push or deployment performed.
