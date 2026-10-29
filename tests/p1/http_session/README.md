# HTTP request lifecycle and session concurrency regressions

Run from the repository root:

```sh
bash tests/p1/http_session/run_tests.sh normal all
bash tests/p1/http_session/run_tests.sh tsan all
bash tests/p1/http_session/run_tests.sh asan all
```

The runner compiles the current production HTTP request/context and session sources with C++17, four real worker threads, and strict compiler warnings. `tsan` enables ThreadSanitizer; `asan` enables AddressSanitizer and UndefinedBehaviorSanitizer. All binaries and debug files go under `$TMPDIR` (or `/tmp`); `P1_TEST_OUTPUT` and `CXX` can override the output directory and compiler. Run the script with `bash`; no executable-bit change is required.

Optional cases are `http`, `storage`, `data`, `expiry`, `manager`, and `manager-pointer`. The HTTP case verifies that a connection owning an HTTP context can be released without calling reset, that a request only observes its connection, that swap exchanges body/length/connection, and that a zero-body keep-alive request cannot inherit its predecessor's body. The session cases exercise concurrent save/load/remove and expiry eviction, independent and shared data access including clear, expiry refresh/read, creation and cookie recovery, ID generation, manager-pointer access, auto-save and destroy. Each concurrency case uses four synchronized worker starts; the manager case creates and verifies 1,600 session IDs.

The baseline `de1731b` failed the HTTP case with nine explicit assertions. The `storage`, `data`, `expiry`, `manager`, and `manager-pointer` cases each independently reported a ThreadSanitizer data race before the corresponding production synchronization changes. On the macOS host, TSan terminates with SIGABRT (shell status 134), despite `exitcode=66`; this is a failing run. Symbolization is disabled by default because the sandbox's external symbolizer is unavailable.

Only Muduo's I/O shell types (connection, context ownership, timestamp, and buffer) are stubbed. Parsing, request reset/swap, session state, storage and manager behavior use repository sources. These tests do not start a live Muduo listener or validate the full application, providers, database, browser, packaging or deployment. The existing lifetime rule still requires the session manager to outlive requests using its sessions; the tests keep their manager alive until all worker threads join.
