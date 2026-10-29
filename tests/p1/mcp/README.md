# MCP/SSE and chat-entry regression

Run from the repository root (real `nlohmann/json.hpp` must be available under
`MYCPP_TEST_DEPS`, default `/private/tmp/mycpp-p1-deps`):

```sh
python3 tests/p1/mcp/run.py
MCP_TEST_CXXFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' python3 tests/p1/mcp/run.py
MCP_TEST_CXXFLAGS='-fsanitize=thread -g' python3 tests/p1/mcp/run.py
```

This runs 28 cases across three independently compiled fixtures. Each Python
entry point extracts the complete current production function body without
rewriting it. Temporary compiled sources and binaries are removed after the run.

- `helper_extended.py`: actual `chat`, `chatStream`, `commitExchange`,
  `GetMessages`, `restoreMessage`, `request`, and `setStrategy`. The actual `ChatMessage.h`
  and real JSON library validate admitted queue bodies. Tests cover temporary
  MCP context, tool/phase-two failure, unchanged prior history, actual single-item
  exchange serialization, rejected queue admission, empty responses, sync/stream
  user/session key namespacing and full local history, concurrent paired history,
  restored timestamp overflow, and custom requests on a default helper.
  Cloud/model/tool/MQ/entropy are external
  substitutes; this suite does not confirm broker delivery or disk persistence.
- `handlers.py`: actual complete `ChatSendHandler::handle` and
  `ChatCreateAndSendHandler::handle`. Fake HTTP/session/connection and AIHelper
  boundaries exercise foreign/unknown/empty ids, authentication, valid owned
  sync/SSE routing, content events, and creation of a new owned id rather than
  adopting the caller's requested id. No real sockets or cookie backend are used.
- `curl_stream.py`: actual `executeCurlStream` and actual
  `AIStrategy::parseStreamChunk`; fake libcurl feeds fragmented bytes to the real
  write callback. Tests require exactly one success or error result for completed
  streams, premature EOF, JSON error events, transport failure, HTTP failure and
  duplicate terminal markers. No network request is sent.

The parser fixture first reproduced four RED failures: silent EOF, silent JSON
error, HTTP 503 treated as success, duplicate completion callback. The exact
commit fixture also first reproduced acceptance after `LLONG_MAX` restored
timestamp overflow. The parent production fixes made those cases GREEN.

These are source-function regressions at explicit external boundaries. They are
not a complete application build, real provider/model/broker validation, browser
acceptance, package validation or release evidence.
