# Gemma pool regression

Run from the repository root:

```sh
bash tests/p1/gemma/run.sh
GEMMA_TEST_CXXFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' bash tests/p1/gemma/run.sh
GEMMA_TEST_CXXFLAGS='-fsanitize=thread -g' bash tests/p1/gemma/run.sh
```

The script compiles the production `GemmaInferencePool.cpp` with a minimal fake
engine at the external dependency boundary. No model weights or model service
are loaded. Temporary files are nonempty placeholders accepted only by the fake.

The initial production-source RED failed missing-file, bounded-cache and token
budget assertions. The clear/shutdown concurrency probes crashed with Bus error
on the old source because active inference retained pointers to destroyed state.
After the fixes, all cases pass. A further mixed incremental/history regression
first failed because an empty tracked transcript incorrectly matched a one-turn
history; that case now forces a fresh cache.

## Policy and context contract

- Default: 2 live KV caches, sequence length 1024, idle expiry after 10 minutes,
  generation limit 256. `initialize(weights, tokenizer, CachePolicy)` configures
  retention before loading. Sequence length must be a multiple of 32, between
  32 and 8192; count and TTL must be positive.
- LRU eviction happens before allocation, so even the allocation peak obeys the
  count bound. Idle expiry is checked lazily when the next generation starts.
- For Gemma 2 2B's 26 layers, 4 KV heads and 256 head dimension, the checked fork's
  original three-array KV layout has about 208 MiB of raw payload per 1024-token
  cache (416 MiB for two), plus allocation padding. This is a calculated cache
  estimate, not a measured process-RSS cap. Model weights and inference workspaces
  are additional. Different model layouts can use different memory.
- Use `generateFromHistory` or `generateSyncFromHistory` for application calls.
  Supply the canonical transcript, alternating user/assistant, ending in the new
  user message. Timestamps are ignored. KV reuse requires the complete preceding
  text to match the pool's completed transcript exactly. On expiry, eviction,
  clear, or changed history, prior users are wrapped with the fork's chat template
  and assistant texts are tokenized into the replay. No turn is silently dropped.
- `generate`/`generateSync` remain incremental compatibility methods. They cannot
  recover missing history after cache eviction and should not back application
  conversation persistence. Moving from these methods to a History method
  always rebuilds the context.
- The pool rejects tokenization errors, invalid generation settings and any
  prompt + past position + requested generation that exceeds the actual cache
  capacity. An oversized transcript returns an error; callers must ask the user
  to shorten it or deliberately establish a separate summarization policy.
- One lock owns loading, inference, clear, eviction and shutdown. Token callbacks
  run under this lock and must not call back into the pool. A thrown callback or
  engine exception discards its partially mutated KV state.

## Dependency boundary

The API and template/capacity assumptions were checked against
[`qiuku30/gemma.cpp` at `6abb6b1f5545d7d0059f2ed31afb095d655c9b73`](https://github.com/qiuku30/gemma.cpp/tree/6abb6b1f5545d7d0059f2ed31afb095d655c9b73):
`gemma/gemma_args.h`, `gemma/tokenizer.cc`, `gemma/kv_cache.cc`, and
`gemma/gemma.cc`. A deployed library must match that interface.

File checks reject missing, empty, non-regular or unreadable paths before
`InternalInit`/loader entry. Token checks reject known oversize-prompt abort
inputs before `Generate`. They do not prove arbitrary corrupted model bytes,
tokenizer corruption, a file replacement race, allocation failure or unrelated
third-party `HWY_ABORT` paths are isolated. Those require actual dependency/model
validation or process isolation. This suite proves production pool behavior at
the fake boundary, not successful real-model inference or deployment.
