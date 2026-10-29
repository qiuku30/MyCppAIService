# P2 regression continuation

Run from the repository root, with the same real nlohmann/json and OpenSSL
setup described in `tests/p1/README.md`. Python 3, Clang/C++17, Node, CMake and
Ruby (Compose YAML validation) must be available.

```sh
export MYCPP_TEST_DEPS=/tmp/mycpp-p1-deps
bash tests/p2/run_all.sh normal
bash tests/p2/run_all.sh asan
bash tests/p2/run_all.sh tsan
```

The entrypoint runs all P1 regressions first, then HTTP, history, RAG and deploy
regressions. ASan mode also enables UBSan. TSan mode instruments the P1 threaded
slices, history and RAG fixtures; HTTP dispatch and deployment checks run normally.
Fixture cases, C++ assertions, subprocesses and syntax checks are different
units and should not be combined into one reported homogeneous test count.

## Reviewed P2 mapping

| Original finding | Current repair and regression |
| --- | --- |
| Keep-alive body residue | P1 complete request swap retained; P1 and P2 HTTP tests |
| Header spelling and loose Content-Length | Case normalization, strict bounded decimal and unambiguous framing; `http/` |
| CORS/default error status line | Valid version/status/framing and correct bodyless responses; `http/` |
| Failure still saves/calls success, role parity drift | P1 request snapshots and paired commit retained; P1 MCP tests and explicit restored-role guard in `history/` |
| Frontend ignores SSE error | P1 frontend regression retained; `tests/p1/frontend/` |
| Query ResultSet leak | P1 callback-scoped result/statement/lease RAII retained; `tests/p1/db_auth/` |
| Restart history disabled | Staged all-or-nothing load before serving, exact stored role/content/timestamp snapshots, retry replacement and read-only lookup; `history/` |
| Deployment and RAG configuration mismatch | Portable dependency discovery, pinned current dependencies, selected-DB initialization wiring and consistent model mount paths; `deploy/`, `rag/` |

Additional reproduced adjacent repairs include coalesced request dispatch,
terminal HTTP input disposal after close/error/SSE takeover, EventLoop member
lifetime, immutable history lookup, shared session index locking, strict RAG
event shape and curl setup resource cleanup. Each has RED evidence in the scope
README or implementation progress log.

## Evidence boundaries and rollout

These suites exercise production source or exact production function bodies;
external sockets, model engine, database and provider transports use explicit
fixtures. History restart tests use separate processes with a disk-backed fake
database; they do not establish real MySQL durability. Deployment checks parse
YAML/shell and generate the actual CMake project with declared dependency
placeholders; those placeholders are not compiled or linked.

Existing P1 schema migrations still apply. A new empty MySQL volume uses the
selected database and mounted `deploy/init.sql`; existing volumes must be
migrated explicitly, preserving their data. See `deploy/README.md` and
`tests/p1/README.md`. Optional image recognition assets use the separate
`deploy/docker-compose.image.yml` override, not an implicit download or a
required mount for cloud-only calls.

The patch does not establish real Linux linking/image startup, provider/model
inference, TLS, UI acceptance or broker/disk/restart guarantees. Model loader
abort containment, durable local outbox and throughput redesign remain separate
from the reviewed P2 list. The actual host build/configure result is recorded in
`docs/superpowers/plans/2026-10-29-p2-correctness.md`.
