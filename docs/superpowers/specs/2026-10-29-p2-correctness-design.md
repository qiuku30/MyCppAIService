# P2 correctness continuation

User explicitly authorizes the remaining reviewed P2 repairs and tests. Preserve all uncommitted P1 changes and existing data; baseline captured under /private/tmp/mycpp-p2-baseline-20261029. No commit/push/deployment or production database mutation.

## Scope

- HTTP headers compare case-insensitively; Content-Length must be a fully consumed nonnegative decimal with a bounded body. Reject conflicting/unsupported framing rather than accepting ambiguous messages. Every CORS/default-error response has a valid HTTP status line and framing.
- Restore database history before serving requests. Stage restore state so failures/retries cannot publish partial or duplicate history. Preserve user/session ownership and stored content/metadata. Verify exact replay after restart with offline source tests and mark the real-DB boundary.
- Fix RAG environment naming and request/response streaming semantics against the primary API docs; no credentials or real provider calls in tests.
- Close build/deployment configuration mismatches: discover dependencies portably, use current AMQP-CPP, consistent Gemma/container paths, selected DB schema initialization, reproducible Linux instructions and validation checks.
- Existing P1 coverage for request reset, failed streams and scoped query results remains mandatory. Previously suggested throughput redesign, outbox, arbitrary third-party abort containment, benchmarks and unverified HTTPS internals are not newly labelled proven P2 defects.

## Verification

Write reproducible failing tests against the current P1 tree before production changes; implement minimal fixes; run P2 regressions and full P1 suites. Use real headers/libraries when available; do not confuse generated fixtures, syntax checks or static deployment checks with a real Linux image, database/broker or model execution. Document any remaining unavailable integration boundary.
