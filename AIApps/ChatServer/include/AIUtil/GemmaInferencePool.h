#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

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
    static GemmaInferencePool& instance();

    // 加载模型（启动时调用一次）
    // weights_path: .sbs 模型文件路径
    // tokenizer_path: .spm tokenizer 文件路径（为空则尝试从 .sbs 内嵌加载）
    // 失败时抛 std::runtime_error
    void initialize(const std::string& weights_path,
                    const std::string& tokenizer_path = "");

    bool isLoaded() const { return gemma_ != nullptr; }

    // 流式推理：逐 token 回调 on_token（在调用线程中执行）
    // session_id 用于获取/创建 per-session KV Cache，支持多轮对话
    void generate(const std::string& session_id,
                  const std::string& prompt,
                  std::function<void(const std::string& token)> on_token,
                  float temperature = 0.7f,
                  size_t max_tokens = 1024);

    // 非流式推理：返回完整生成文本
    std::string generateSync(const std::string& session_id,
                             const std::string& prompt,
                             float temperature = 0.7f,
                             size_t max_tokens = 1024);

    // 清理指定 session 的 KV Cache
    void clearSession(const std::string& session_id);

    void shutdown();

private:
    GemmaInferencePool() = default;
    ~GemmaInferencePool();
    GemmaInferencePool(const GemmaInferencePool&) = delete;
    GemmaInferencePool& operator=(const GemmaInferencePool&) = delete;

    struct SessionData {
        std::unique_ptr<gcpp::KVCache> kv_cache;
        size_t abs_pos = 0;  // 当前序列总 token 数（用于多轮对话位置跟踪）
    };

    SessionData* getOrCreateSession(const std::string& session_id);

    std::unique_ptr<gcpp::Gemma> gemma_;
    std::unique_ptr<gcpp::ThreadingContext> ctx_;
    std::unique_ptr<gcpp::MatMulEnv> matmul_env_;

    std::mutex sessions_mutex_;
    std::unordered_map<std::string, std::unique_ptr<SessionData>> sessions_;

    std::mutex inference_mutex_;  // 串行化 Generate 调用
};
