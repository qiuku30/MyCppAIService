# Transactional startup history restoration

Run from the repository root with real `nlohmann/json.hpp` available under
`MYCPP_TEST_DEPS` (default `/private/tmp/mycpp-p1-deps`):

```sh
python3 tests/p2/history/run.py
HISTORY_TEST_CXXFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' python3 tests/p2/history/run.py
HISTORY_TEST_CXXFLAGS='-fsanitize=thread -fno-omit-frame-pointer' python3 tests/p2/history/run.py
```

The driver compiles the complete current `AIHelper` and `ChatServer` class
declarations and unmodified production bodies for history loading, history
responses, helper restore/snapshot/chat/commit operations, startup dependency
waiting, and `main`. Provider/model, database, HTTP/session, MQ and entropy
boundaries are explicit offline substitutes. Temporary builds and disk fixtures
are removed after each run. `MYCPP_TEST_SOURCE_ROOT` can point at a saved source
snapshot to reproduce the pre-fix failures without changing the checkout.

The suite has 25 cases/scenarios:

- Eighteen history cases check exact content bytes, stored timestamps and roles,
  user/session ownership, deterministic session lists, repeated complete loads,
  successful empty snapshots, query/traversal/row-decoding/completion failures,
  retry without partial publication or duplication, integer user-id bounds,
  invalid roles, legacy session behavior, unchanged failed queue admission,
  full local-model context, concurrent coherent snapshots and lookup-only unknown/
  foreign history requests that cannot create caller-supplied sessions.
- Two fresh fixture processes independently read the same disk-backed offline
  database snapshot, compare exact restored role/content/timestamp metadata,
  check user isolation and pass the full restored context to the local-model
  boundary. This verifies process reconstruction with fixture storage, not MySQL
  durability, broker delivery or real Gemma generation.
- One deterministic session-index fixture compiles the full production create
  handler with a lock-aware index boundary and requires creation to hold the same
  index mutex used by session-list readers. The P1 handler fixture supplies the
  remaining HTTP/session/model boundaries; none of its assertions are changed.
- Four startup scenarios check successful restore before serving, two transient
  query failures followed by success, permanent query failure after all thirty
  attempts with no serving, and constructor initialization failure with a clear
  error and exit status 1. Sleeps are replaced only at the fixture boundary.

On the captured P1 source snapshot, the eighteen history cases produced
fifteen expected failures; both process replay scenarios, the session-index
fixture and all four startup scenarios also fail. The final source passes all
twenty-five normally and under
ASan/UBSan and TSan on the development host.

Database rows are staged in new helpers and two new maps. Query consumption,
row decoding, ownership/role validation and helper construction must finish
before both live maps are swapped under their locks. A retry replaces the
snapshot. No row error is skipped and no database row is rewritten or deleted.
Rows follow the existing database `ORDER BY ts, id`. Equal timestamps for the
same user have no stored sequencing guarantee; no legacy chronology is invented
for those ties. Roles `0` and `1` are preserved exactly, including legacy assistant-first,
adjacent-role or unfinished histories. Such a session remains readable with
its original metadata; generation in that session fails clearly because the
current model interfaces infer alternating roles. Other sessions can continue.
Values outside the database role contract `0`/`1`, and user ids that cannot be
represented by the existing user-key type, reject the whole restore rather
than silently changing ownership or roles.

The real `ChatServer` constructor eagerly initializes the connection pool before
the history retry loop. Constructor failures now produce a clear startup error
and exit; HTTP/EventLoop construction is not retried. After pool initialization,
history-query failures use the bounded retry loop, and exhausted retries cancel
startup. Live MySQL readiness, Connector/C++ result behavior, actual database
restart, RabbitMQ delivery and real model execution remain integration boundaries.
