# HTTP parser, framing and lifetime regressions

Run from the repository root:

```sh
bash tests/p2/http/run.sh normal all
bash tests/p2/http/run.sh asan all
```

The suite compiles the current production `HttpRequest`, `HttpContext`, `HttpResponse`, `HttpServer`, router, middleware chain, CORS and SSL configuration sources. It uses Clang with C++17 and strict warnings. `asan` enables AddressSanitizer and UndefinedBehaviorSanitizer. Binaries go under `$TMPDIR/mycpp-p2-http-<mode>` (or `/tmp`); `P2_HTTP_OUTPUT`, `CXX`, and `OPENSSL_PREFIX` can override the output directory, compiler, and OpenSSL header location. The default OpenSSL prefix is `/opt/homebrew/opt/openssl@3`; no OpenSSL library is linked.

Optional case selectors are `parser`, `response`, `cors`, `server`, `lifetime`, and `dispatch`. The suite has 100 checks:

- Header lookup ignores ASCII case; lower-case Content-Length, Content-Type and Cookie work. Invalid field names and control bytes are rejected.
- Content-Length must contain only decimal digits after optional SP/HTAB trimming. Overflow, lengths above 10 MiB, conflicting duplicate fields, comma lists and unsupported Transfer-Encoding are rejected. Identical duplicate length fields are accepted. The existing requirement for explicit POST/PUT length remains.
- Declared bodies are consumed for every supported method, including fragmented input, and cannot become the next request. Exact 10 MiB bodies and P1 reset isolation remain valid.
- Default, code-only, CORS, 400, 404 and 500 responses have complete status/version/framing. Ordinary responses get one exact Content-Length. A 204 response has neither content nor Content-Length; streaming responses get no synthesized length.
- Connection token matching ignores case and accepts comma-separated tokens; close takes precedence. Ordinary keep-alive responses declare their body length.
- The EventLoop member is constructed before TcpServer and survives its destruction. A registry checks member lifetime without dereferencing unconstructed/destroyed objects.
- Coalesced ordinary requests dispatch in wire order during one message callback. Partial requests return without spinning and finish on later input. Close, handler shutdown, exceptions, malformed input and SSE takeover permanently stop later HTTP dispatch on the connection, including future callbacks. A request close token takes precedence over a normal handler's keep-alive setting. Terminal input is discarded so stopped connections cannot retain an increasing unread HTTP buffer; partial ordinary input remains available.

The original 54 checks compiled against the pre-P2 production snapshot with P1 changes retained and failed 43 explicit assertions (exit 1). The binary and log were saved at `/private/tmp/mycpp-p2-http-red` and `/private/tmp/mycpp-p2-http-red.log`. Those original fixes passed 54/54 in normal and ASan/UBSan modes.

Coalesced-request dispatch was then reproduced as an additional P2 issue and separately authorized. Before its production changes, the 34 added dispatch checks compiled and failed 19 assertions (exit 1); `/private/tmp/mycpp-p2-http-dispatch-red` and `/private/tmp/mycpp-p2-http-dispatch-red.log` preserve that RED. A further 12 checks exposed retained terminal input on stopped connections (12 failures before that correction), preserved in `/private/tmp/mycpp-p2-http-stop-buffer-red` and `/private/tmp/mycpp-p2-http-stop-buffer-red.log`. The final dispatch group has 46 checks, and the combined suite passes 100/100 in normal and ASan/UBSan modes. The P1 HTTP suite also passes with the weak connection reference, complete swap and request-reset behavior retained.

Only the Muduo I/O shell, logging and Boost Any type are replaced by in-process fixtures. The server fixture drives the real connection/message callbacks and captures response bytes; it opens no socket and starts no service or I/O thread. TLS is disabled, and every TLS link stub aborts if entered. This evidence does not validate a real Muduo listener, live TLS, a browser, dependencies outside this suite, packaging or deployment.

The member-order contract is grounded in Muduo v2.0.2 `muduo/net/TcpServer.cc` (its destructor calls `loop_->assertInLoopThread()`) and `Acceptor.cc` (destruction removes the accept channel). The fixture proves the ordering contract; it does not establish a live startup crash.

Protocol references: [RFC 9112 framing](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3), [RFC 9112 field parsing](https://www.rfc-editor.org/rfc/rfc9112.html#section-5.1), and [RFC 9110 Content-Length](https://www.rfc-editor.org/rfc/rfc9110.html#section-8.6).
