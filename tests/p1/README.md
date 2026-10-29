# P1 regression and rollout

Run from the repository root. Requires C++17 clang, Python 3, Node, OpenSSL development headers/library and nlohmann/json. No database, broker or model service is contacted by these tests.

```sh
export MYCPP_TEST_DEPS=/tmp/mycpp-p1-deps
mkdir -p "$MYCPP_TEST_DEPS/nlohmann"
curl -fL https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp -o "$MYCPP_TEST_DEPS/nlohmann/json.hpp"
bash tests/p1/run_all.sh normal
bash tests/p1/run_all.sh asan
bash tests/p1/run_all.sh tsan
```

Set `OPENSSL_PREFIX` for a nonstandard installation. The runner finds Homebrew OpenSSL on macOS and otherwise uses `/usr`. Test builds live in temporary directories. Normal source tests, ASan/UBSan and TSan prove different properties; the TSan mode runs DB/auth and scalar persistence cases normally while instrumenting threaded HTTP/Session, Gemma, MQ and extended AIHelper cases. Every mode includes syntax checks of real JSON/curl/OpenSSL integration with offline service fixtures. These checks are not a complete Linux application build or ABI integration test.

Tests exercise production source directly or extract exact function bodies. Only network transports, database driver, and model engine boundaries are replaced. They cover connection release/reset, Session races, connection leasing/recovery/RAII, password hash/legacy upgrades, bounded Gemma cache/replay/budgets/lifecycle, MCP history/SSE, queue admission, publisher confirms, channel generations, retries/quarantine, parameterized exchange writes, and frontend error handling. Read the per-directory READMEs for evidence and limits.

## Existing data rollout

Stop the old server and back up the existing application database. Apply to that selected database:

1. `deploy/auth-password-migration.sql` widens the password column for the hash encoding. Inspect wider existing columns before applying; do not narrow them. Existing usernames must have a unique index.
2. `deploy/migrations/2026-10-29-chat-message-id.sql` adds nullable stable message IDs and a unique index. Apply once. Old rows keep NULL IDs and their data remains unchanged.

`deploy/init.sql` is the equivalent complete schema for a new selected database. No SQL script is executed automatically by this repair. The P2 dependency/path and initialization wiring updates are described in `deploy/README.md`; an actual Linux image build remains a separate validation step.

Existing plaintext passwords are upgraded only after an exact successful login; see `deploy/auth-password-migration.md`. Older server versions cannot verify upgraded hashes. Do not roll back the old plaintext-only binary against the upgraded data.

New queue payloads are versioned JSON containing one completed question/answer exchange. The consumer writes both rows in one parameterized INSERT. Stable `/u` and `/a` message IDs make redelivery idempotent. Legacy raw SQL backlog is never executed: it is published persistently to `sql_queue.dead`, and the original is acknowledged only after its replacement is confirmed. Retain that queue for manual, reviewed migration; do not purge it.

DB failures retry with bounded backoff (1/2/4/8/16 seconds), then move to the durable dead queue using the same confirm-before-ACK rule. Recovery/replay from that queue is an operational step. MQ queue admission is explicitly bounded at 10,000 including cross-thread queued and unconfirmed work. Admission into memory is not a broker/disk receipt: process termination before publisher confirmation can still lose those unconfirmed new submissions. A durable local outbox would be needed to extend the guarantee across that boundary. Broker-confirmed messages use persistent delivery and durable queues; actual broker/disk failure behavior still requires integration fault injection.

Gemma defaults to two 1024-token caches, lazy idle expiry after ten minutes, and generation limit256. Canonical history is replayed on eviction/expiry/model-history changes. Oversized context is rejected rather than silently truncated. Missing/empty/unreadable model paths are checked before loader entry; malformed nonempty third-party model files and arbitrary engine aborts still require real-model validation or process isolation. See `gemma/README.md`.
