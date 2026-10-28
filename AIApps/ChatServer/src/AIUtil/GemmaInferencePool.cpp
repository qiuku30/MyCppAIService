#include "../include/AIUtil/GemmaInferencePool.h"

#include <stdexcept>
#include <vector>

#include "gemma/gemma.h"
#include "gemma/gemma_args.h"
#include "gemma/kv_cache.h"
#include "gemma/tokenizer.h"
#include "ops/matmul.h"
#include "util/threading_context.h"

GemmaInferencePool& GemmaInferencePool::instance() {
    static GemmaInferencePool pool;
    return pool;
}

GemmaInferencePool::~GemmaInferencePool() {
    shutdown();
}

void GemmaInferencePool::initialize(const std::string& weights_path,
                                    const std::string& tokenizer_path) {
    if (gemma_) {
        return;  // 已加载
    }

    // 初始化 Highway SIMD 运行时
    gcpp::InternalInit();

    // 构建参数
    gcpp::LoaderArgs loader(tokenizer_path, weights_path);

    gcpp::ThreadingArgs threading;
    gcpp::InferenceArgs inference;
    inference.max_generated_tokens = 1024;
    inference.temperature = 0.7f;

    gcpp::GemmaArgs args(loader, threading, inference);

    // 创建 ThreadingContext（管理线程池）
    ctx_ = std::make_unique<gcpp::ThreadingContext>(args.threading);

    // 创建 MatMulEnv（矩阵乘法环境）
    matmul_env_ = std::make_unique<gcpp::MatMulEnv>(*ctx_);

    // 加载模型
    gemma_ = std::make_unique<gcpp::Gemma>(args, *ctx_);
}

void GemmaInferencePool::shutdown() {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.clear();
    gemma_.reset();
    matmul_env_.reset();
    ctx_.reset();
}

GemmaInferencePool::SessionData*
GemmaInferencePool::getOrCreateSession(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        return it->second.get();
    }
    if (!gemma_) {
        throw std::runtime_error("Gemma model not loaded");
    }
    auto session = std::make_unique<SessionData>();
    session->kv_cache = std::make_unique<gcpp::KVCache>(
        gemma_->Config(), gemma_->Inference(), ctx_->allocator);
    session->abs_pos = 0;
    auto* ptr = session.get();
    sessions_[session_id] = std::move(session);
    return ptr;
}

void GemmaInferencePool::clearSession(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.erase(session_id);
}

void GemmaInferencePool::generate(
    const std::string& session_id,
    const std::string& prompt,
    std::function<void(const std::string& token)> on_token,
    float temperature,
    size_t max_tokens) {

    if (!gemma_) {
        throw std::runtime_error("Gemma model not loaded");
    }

    // 串行化 Generate 调用（模型内部线程池不可重入）
    std::lock_guard<std::mutex> lock(inference_mutex_);

    auto* session = getOrCreateSession(session_id);

    // Tokenize + 对话模板包装
    std::vector<int> tokens = gcpp::WrapAndTokenize(
        gemma_->Tokenizer(), gemma_->ChatTemplate(),
        gemma_->Config().wrapping, session->abs_pos, prompt);

    const size_t prompt_size = tokens.size();
    const size_t start_pos = session->abs_pos;
    size_t generated = 0;

    // 构建 RuntimeConfig
    gcpp::RuntimeConfig runtime;
    gemma_->Inference().CopyTo(runtime);
    runtime.max_generated_tokens = max_tokens;
    runtime.temperature = temperature;
    runtime.verbosity = 0;

    runtime.stream_token = [&](int token, float /*prob*/) -> bool {
        ++generated;
        // Prefill 阶段：跳过回调，只计数
        if (generated <= prompt_size) {
            return true;
        }
        // 遇到 EOS 则停止
        if (gemma_->Config().IsEOS(token)) {
            return true;
        }
        // Decode token → 文本，回调
        std::string token_text;
        if (gemma_->Tokenizer().Decode({token}, &token_text) && on_token) {
            on_token(token_text);
        }
        return true;
    };

    gcpp::TimingInfo timing_info;

    // 执行推理（阻塞）
    gemma_->Generate(runtime, gcpp::PromptTokens(tokens.data(), tokens.size()),
                     start_pos, /*prefix_end=*/0,
                     *session->kv_cache, *matmul_env_, timing_info);

    // 更新 session 位置用于下一轮对话
    session->abs_pos = start_pos + generated;
}

std::string GemmaInferencePool::generateSync(
    const std::string& session_id,
    const std::string& prompt,
    float temperature,
    size_t max_tokens) {

    std::string full_text;
    generate(session_id, prompt,
             [&full_text](const std::string& token) {
                 full_text += token;
             },
             temperature, max_tokens);
    return full_text;
}
