# P1 Hardening Implementation Plan

> Execution: scoped parallel agents for independent HTTP/session, DB/auth, Gemma tasks; parent implements MCP/SSE and MQ and reviews all changes.

Goal: repair reviewed P1 defects and deliver repeatable regression tests.
Architecture: retain muduo HTTP and strategy interfaces, enforce explicit ownership and failure state; keep model and broker boundaries visible.
Tech stack: C++17, OpenSSL, muduo, AMQP-CPP, MySQL Connector/C++, Gemma fork.

## Constraints

User explicitly authorizes fixes and tests. No production DB/broker/model access, no commit/push/deploy. Preserve .serena and existing data. Tests must exercise production source; mocks only at external boundaries. No unrelated feature additions.

## Tasks

- [x] HTTP/session: regression for weak connection release/reset and multithreaded session access; fix references, complete request reset and synchronization; run normal and TSan regressions.
- [x] DB/auth: fake-driver RED for exclusive health checks and recovery capacity; fix pool ownership/lifecycle. Add password hash/verify tests; parameterize auth queries, migrate legacy password only on successful login, retain data. Release ResultSets safely.
- [x] Gemma: fake-engine RED for user-separated keys, bounded retention, missing files and token budget; implement bounded session policy, serialize clear/shutdown with inference, validate before third-party abort paths.
- [x] MCP/SSE: RED for exact history and nonempty wire answer, tool exception and cloud error; use request-local context, send final content, suppress success after failure, remove sensitive logging.
- [x] MQ: RED against fake EventLoop/Channel/AMQP contracts for multi-fd removal, callback destruction, persistent publish, confirms, retries and no ACK on failed DB/fallback. Implement lifecycle and publisher state machine; structured parameterized writes with idempotency migration.
- [x] Integration: review cross-component signatures and dependencies; execute all regression commands, sanitizer checks where supported, attempt real Linux dependencies if available, record unavailable environments honestly.
- [x] Delivery: review diff, preserve unrelated files, document migration and test results, enumerate remaining integration limits.

## Final validation (2026-10-29)

All three unified runs exited 0 after the final production changes:

- `MYCPP_TEST_DEPS=/private/tmp/mycpp-p1-deps bash tests/p1/run_all.sh normal` — `/private/tmp/mycpp-p1-all-final-normal.log`
- `MYCPP_TEST_DEPS=/private/tmp/mycpp-p1-deps bash tests/p1/run_all.sh asan` — `/private/tmp/mycpp-p1-all-asan.log`
- `MYCPP_TEST_DEPS=/private/tmp/mycpp-p1-deps bash tests/p1/run_all.sh tsan` — `/private/tmp/mycpp-p1-all-tsan.log`

Coverage includes HTTP/Session lifetime and synchronization; 8 DB/auth executable cases; 13 Gemma cases; 81 MQ assertions; 28 AIHelper/MCP/SSE/handler cases; 5 persistence cases; 4 frontend cases; 5 real-JSON/curl/OpenSSL integration translation units. Counts mix cases/assertions/executables and must not be added as a single homogeneous test count. TSan mode instruments the threaded slices; see tests/p1/README.md for each mode's boundary.

MQManager.cpp also passes `-fsyntax-only` with actual AMQP-CPP headers from official commit839f74f2e7cc0f3f03458ebf943d23fc1f8c5462, real JSON/OpenSSL and the minimal muduo fixture. A further check using real muduo2.0.2 headers stops at missing Boost headers on this host.

Full application configure was attempted in `/private/tmp/mycpp-p1-app-configure`; CMake generation failed because ONNXRUNTIME_LIBRARY is unavailable. This macOS host has no Docker/Linux runner, real model, database or broker. No full Linux build, real inference, broker restart/disk persistence, or production schema migration has been verified. Real-provider RAG streaming, remaining HTTP P2 parsing/CORS, deployment dependency paths, UI acceptance and benchmarking are outside this P1 patch.

Rollout requires `deploy/auth-password-migration.sql` and `deploy/migrations/2026-10-29-chat-message-id.sql` against the selected existing database. These scripts were not executed against user data. Existing raw SQL backlog goes to confirmed durable quarantine instead of execution; retain it for reviewed migration. In-memory publisher admission is not a disk receipt, and nonempty corrupt Gemma files remain outside the validated precheck guarantee.

Cross review found and fixed the actual3-argument AMQP NACK callback, mandatory-return routing, strict integer schema version, restored timestamp overflow, and custom request default strategy. Valid RED runs preceded fixes. `git diff --check` passed. No commit or push performed.
