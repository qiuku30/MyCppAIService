# RAG application adapter regression

Run `PYTHONDONTWRITEBYTECODE=1 python3 tests/p2/rag/run.py` from the root. Set
`MYCPP_TEST_DEPS` to a directory containing real `nlohmann/json.hpp`; on this
host it is `/private/tmp/mycpp-p1-deps`. `P2_TEST_CXXFLAGS` can enable sanitizers.

The suite compiles the real AIStrategy/AIFactory sources and extracts the exact
current AIHelper curl methods. Fake libcurl only supplies response bytes and
captures request headers/body; it never makes network requests. All credentials
and IDs in the fixture are synthetic. Twenty-six checks cover variable precedence,
legacy compatibility, invalid app IDs, proper HTTP incremental negotiation,
fragmented delta/terminal frames, provider errors, premature end, ordinary cloud
compatibility allocation/config error ordering, malformed text rejection and all header-allocation failure positions with resource balance checks.

`KNOWLEDGE_BASE_ID` is the canonical historical configuration name, but its value
is a DashScope **application ID**, not a knowledge-base/pipeline ID. The old
`Knowledge_Base_ID` spelling works only when the canonical variable is absent;
an explicitly empty canonical value fails rather than falling back silently.
The endpoint remains the Beijing application-completion endpoint used by this
project. This adapter handles documented `output.text` application responses,
requests incremental output, and requires a natural `finish_reason:stop` or the
ordinary terminal marker. It does not turn workflow node snapshots into text or
silently treat cancellation/length limits as a successful answer.

Primary contract checked 2026-10-29:
- https://help.aliyun.com/zh/model-studio/agent-and-workflow-application-api-reference
- https://help.aliyun.com/zh/model-studio/call-single-agent-application

The original current-tree RED had fourteen failed checks. Additional failure
classification and malformed chunk cases were RED before refinement. Passing
these source tests does not establish a successful real-provider request.
