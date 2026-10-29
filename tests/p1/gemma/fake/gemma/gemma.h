#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

// External engine boundary only: GemmaInferencePool.cpp remains production code.
namespace gcpp {
inline std::atomic<int> live_caches{0}, next_cache{0}, generate_calls{0}, init_calls{0};
inline std::atomic<int> observed_cache{0};
inline std::atomic<int> peak_caches{0};
inline size_t observed_capacity = 0;
inline size_t model_sequence_limit = 8192;
inline size_t observed_pos = 0;
inline std::vector<int> observed_tokens;
inline bool block_generate = false, entered_generate = false, release_generate = false;
inline std::atomic<bool> generating{false}, destroyed_while_generating{false};
inline std::mutex barrier_mutex;
inline std::condition_variable barrier;
inline void InternalInit() { ++init_calls; }
struct RuntimeConfig {
    size_t max_generated_tokens = 0;
    float temperature = 0;
    int verbosity = 0;
    std::function<bool(int, float)> stream_token;
};
struct LoaderArgs { LoaderArgs(const std::string&, const std::string&) {} };
struct ThreadingArgs {};
struct InferenceArgs {
    size_t seq_len = 8192, max_generated_tokens = 0;
    float temperature = 0;
    void CopyTo(RuntimeConfig&) const {}
};
struct GemmaArgs {
    ThreadingArgs threading;
    InferenceArgs inference;
    GemmaArgs(const LoaderArgs&, const ThreadingArgs& t, const InferenceArgs& i)
        : threading(t), inference(i) {}
};
class ThreadingContext { public: int allocator = 0; explicit ThreadingContext(const ThreadingArgs&) {} };
class MatMulEnv { public: explicit MatMulEnv(ThreadingContext&) {} };
struct ModelConfig { int wrapping = 0; bool IsEOS(int token) const { return token == 0; } };
struct GemmaTokenizer {
    bool Encode(const std::string& input, std::vector<int>* out) const {
        if (input == "!encode-failure!") return false;
        out->assign(input.begin(), input.end());
        return true;
    }
    bool Decode(const std::vector<int>&, std::string* out) const { *out = "answer"; return true; }
};
struct KVCache {
    int id;
    size_t seq_len;
    KVCache(const ModelConfig&, const InferenceArgs& i, int)
        : id(++next_cache), seq_len(std::min(i.seq_len, model_sequence_limit)) {
        const int current = ++live_caches;
        if (current > peak_caches) peak_caches = current;
    }
    ~KVCache() { if (generating) destroyed_while_generating = true; --live_caches; }
    size_t SeqLen() const { return seq_len; }
};
struct TimingInfo {};
struct PromptTokens {
    const int* data;
    size_t size;
    PromptTokens(const int* d, size_t s) : data(d), size(s) {}
};
inline std::vector<int> WrapAndTokenize(const GemmaTokenizer& t, int, int, size_t pos, const std::string& input) {
    std::vector<int> out;
    if (!t.Encode(input, &out)) throw std::runtime_error("fake tokenizer assertion");
    out.insert(out.begin(), pos == 0 ? 2 : 3);
    return out;
}
class Gemma {
    InferenceArgs inference_;
public:
    Gemma(const GemmaArgs& a, ThreadingContext&) : inference_(a.inference) {}
    const ModelConfig& Config() const { static ModelConfig c; return c; }
    const InferenceArgs& Inference() const { return inference_; }
    const GemmaTokenizer& Tokenizer() const { static GemmaTokenizer t; return t; }
    int ChatTemplate() const { return 0; }
    void Generate(const RuntimeConfig& r, PromptTokens p, size_t pos, size_t, KVCache& kv, MatMulEnv&, TimingInfo&) {
        ++generate_calls;
        observed_cache = kv.id;
        observed_pos = pos;
        observed_capacity = kv.SeqLen();
        observed_tokens.assign(p.data, p.data + p.size);
        generating = true;
        struct ResetFlag { ~ResetFlag() { generating = false; } } reset_flag;
        {
            std::unique_lock<std::mutex> lock(barrier_mutex);
            entered_generate = true;
            barrier.notify_all();
            barrier.wait(lock, [] { return !block_generate || release_generate; });
        }
        for (size_t i = 0; i < p.size; ++i) r.stream_token(p.data[i], 0);
        r.stream_token(99, 1);
        r.stream_token(0, 1);
    }
};
}
