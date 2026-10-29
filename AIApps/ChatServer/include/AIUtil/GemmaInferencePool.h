#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Forward declarations for gemma.cpp types
namespace gcpp {
class Gemma;
class ThreadingContext;
class MatMulEnv;
struct KVCache;
}  // namespace gcpp

// Gemma 本地推理池：模型单例 + 推理线程池 + per-session KV Cache
// 所有 Generate 调用被 inference_mutex_ 串行化（模型内部已多线程并行）
class GemmaInferencePool {
public:
    using History = std::vector<std::pair<std::string, long long>>;
    struct CachePolicy {
        size_t max_sessions = 2;
        size_t sequence_tokens = 1024;
        std::chrono::milliseconds idle_ttl = std::chrono::minutes(10);
    };

    static GemmaInferencePool& instance();

    // Empty tokenizer_path uses a tokenizer embedded in the weights.
    // Validate files before entering the fork, whose loader may abort on failure.
    void initialize(const std::string& weights_path,
                    const std::string& tokenizer_path = "");
    void initialize(const std::string& weights_path,
                    const std::string& tokenizer_path,
                    const CachePolicy& policy);
    bool isLoaded() const;

    // Incremental compatibility API. A missing/evicted cache starts with prompt
    // only; callers requiring durable conversation context must use History.
    // Callbacks execute under the inference lock and must not reenter this pool.
    void generate(const std::string& session_key,
                  const std::string& prompt,
                  std::function<void(const std::string&)> on_token,
                  float temperature = 0.7f,
                  size_t max_tokens = 256);
    std::string generateSync(const std::string& session_key,
                             const std::string& prompt,
                             float temperature = 0.7f,
                             size_t max_tokens = 256);

    // Canonical history alternates user/assistant and ends with the new user.
    // Timestamps are not model input. Only an exact prior transcript can reuse
    // KV state; misses, expiry and changed history replay the supplied transcript.
    // Oversized history is rejected, never silently truncated.
    void generateFromHistory(const std::string& session_key,
                             const History& history_including_new_user,
                             std::function<void(const std::string&)> on_token,
                             float temperature = 0.7f,
                             size_t max_tokens = 256);
    std::string generateSyncFromHistory(const std::string& session_key,
                                       const History& history_including_new_user,
                                       float temperature = 0.7f,
                                       size_t max_tokens = 256);

    void clearSession(const std::string& session_key);
    void shutdown();

private:
    GemmaInferencePool() = default;
    ~GemmaInferencePool();
    GemmaInferencePool(const GemmaInferencePool&) = delete;
    GemmaInferencePool& operator=(const GemmaInferencePool&) = delete;
    using Clock = std::chrono::steady_clock;
    struct SessionData {
        std::unique_ptr<gcpp::KVCache> kv_cache;
        size_t abs_pos = 0;
        std::vector<std::string> transcript;
        Clock::time_point last_used;
        uint64_t access_order = 0;
    };

    void generateImpl(const std::string& session_key,
                      const std::string& prompt,
                      const History* history,
                      std::function<void(const std::string&)> on_token,
                      float temperature,
                      size_t max_tokens);
    void pruneExpiredLocked(Clock::time_point now);

    std::unique_ptr<gcpp::Gemma> gemma_;
    std::unique_ptr<gcpp::ThreadingContext> ctx_;
    std::unique_ptr<gcpp::MatMulEnv> matmul_env_;
    std::unordered_map<std::string, std::unique_ptr<SessionData>> sessions_;
    CachePolicy policy_;
    uint64_t access_order_ = 0;
    // One ownership lock covers inference, cache eviction, load, clear and
    // shutdown. No cache/model pointer escapes its lifetime.
    mutable std::mutex inference_mutex_;
};
