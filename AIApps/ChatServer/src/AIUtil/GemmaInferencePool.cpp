#include "AIUtil/GemmaInferencePool.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "gemma/gemma.h"
#include "gemma/gemma_args.h"
#include "gemma/kv_cache.h"
#include "gemma/tokenizer.h"
#include "ops/matmul.h"
#include "util/threading_context.h"

namespace {
void validateReadableFile(const std::string& path, const char* kind) {
    std::error_code error;
    if (path.empty() || !std::filesystem::is_regular_file(path, error) ||
        error || std::filesystem::file_size(path, error) == 0 || error) {
        throw std::runtime_error(std::string("Gemma ") + kind +
                                 " must be a nonempty readable regular file");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input || input.peek() == std::ifstream::traits_type::eof()) {
        throw std::runtime_error(std::string("Gemma ") + kind + " is not readable");
    }
}
void validateBudget(size_t start, size_t prompt, size_t generated, size_t capacity) {
    if (prompt == 0 || start > capacity || prompt > capacity - start ||
        generated > capacity - start - prompt) {
        throw std::length_error("Gemma context token budget exceeded; shorten history or generation");
    }
}
}  // namespace

GemmaInferencePool& GemmaInferencePool::instance() {
    static GemmaInferencePool pool;
    return pool;
}
GemmaInferencePool::~GemmaInferencePool() { shutdown(); }

void GemmaInferencePool::initialize(const std::string& weights_path,
                                    const std::string& tokenizer_path) {
    initialize(weights_path, tokenizer_path, CachePolicy{});
}
void GemmaInferencePool::initialize(const std::string& weights_path,
                                    const std::string& tokenizer_path,
                                    const CachePolicy& policy) {
    std::lock_guard<std::mutex> lock(inference_mutex_);
    if (gemma_) return;
    if (policy.max_sessions == 0 || policy.sequence_tokens < 32 ||
        policy.sequence_tokens > 8192 || policy.sequence_tokens % 32 != 0 ||
        policy.idle_ttl.count() <= 0) {
        throw std::invalid_argument("Invalid Gemma cache policy");
    }
    validateReadableFile(weights_path, "weights");
    if (!tokenizer_path.empty()) validateReadableFile(tokenizer_path, "tokenizer");
    gcpp::InternalInit();
    gcpp::LoaderArgs loader(tokenizer_path, weights_path);
    gcpp::ThreadingArgs threading;
    gcpp::InferenceArgs inference;
    inference.seq_len = policy.sequence_tokens;
    inference.max_generated_tokens = std::min(size_t{256}, policy.sequence_tokens / 2);
    inference.temperature = 0.7f;
    gcpp::GemmaArgs args(loader, threading, inference);
    // Publish a complete model atomically; failed construction leaves unloaded.
    auto context = std::make_unique<gcpp::ThreadingContext>(args.threading);
    auto matmul = std::make_unique<gcpp::MatMulEnv>(*context);
    auto model = std::make_unique<gcpp::Gemma>(args, *context);
    ctx_ = std::move(context);
    matmul_env_ = std::move(matmul);
    gemma_ = std::move(model);
    policy_ = policy;
}

bool GemmaInferencePool::isLoaded() const {
    std::lock_guard<std::mutex> lock(inference_mutex_);
    return gemma_ != nullptr;
}
void GemmaInferencePool::shutdown() {
    std::lock_guard<std::mutex> lock(inference_mutex_);
    sessions_.clear();
    gemma_.reset();
    matmul_env_.reset();
    ctx_.reset();
    access_order_ = 0;
}
void GemmaInferencePool::clearSession(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(inference_mutex_);
    sessions_.erase(session_key);
}
void GemmaInferencePool::pruneExpiredLocked(Clock::time_point now) {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (now - it->second->last_used >= policy_.idle_ttl) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

void GemmaInferencePool::generateImpl(
    const std::string& session_key, const std::string& prompt,
    const History* history, std::function<void(const std::string&)> on_token,
    float temperature, size_t max_tokens) {
    std::lock_guard<std::mutex> lock(inference_mutex_);
    if (!gemma_) throw std::runtime_error("Gemma model not loaded");
    if (session_key.empty() || max_tokens == 0 || !std::isfinite(temperature) ||
        temperature < 0.0f) {
        throw std::invalid_argument("Invalid Gemma generation request");
    }
    if (history && (history->empty() || history->size() % 2 == 0)) {
        throw std::invalid_argument("Gemma history must alternate user/assistant and end with user");
    }
    pruneExpiredLocked(Clock::now());
    auto found = sessions_.find(session_key);
    bool reuse = found != sessions_.end();
    if (history && reuse) {
        const auto& prior = found->second->transcript;
        reuse = !prior.empty() && prior.size() + 1 == history->size();
        for (size_t i = 0; reuse && i < prior.size(); ++i) {
            reuse = prior[i] == (*history)[i].first;
        }
    }
    const size_t start_pos = reuse ? found->second->abs_pos : 0;
    std::vector<int> tokens;
    auto append = [&](const std::string& text, bool user_turn) {
        std::vector<int> part;
        // WrapAndTokenize asserts on Encode failure in the fork; check first.
        if (!gemma_->Tokenizer().Encode(text, &part)) {
            throw std::runtime_error("Gemma tokenizer could not encode input");
        }
        if (user_turn) {
            part = gcpp::WrapAndTokenize(
                gemma_->Tokenizer(), gemma_->ChatTemplate(),
                gemma_->Config().wrapping, start_pos + tokens.size(), text);
        }
        if (part.size() > policy_.sequence_tokens - tokens.size()) {
            throw std::length_error("Gemma context token budget exceeded; shorten history");
        }
        tokens.insert(tokens.end(), part.begin(), part.end());
    };
    if (history && !reuse) {
        for (size_t i = 0; i < history->size(); ++i) {
            append((*history)[i].first, i % 2 == 0);
        }
    } else {
        append(prompt, true);
    }
    validateBudget(start_pos, tokens.size(), max_tokens,
                   reuse ? found->second->kv_cache->SeqLen() : policy_.sequence_tokens);
    SessionData* session = nullptr;
    if (reuse) {
        session = found->second.get();
    } else {
        // Erase before allocation so the peak number of KV allocations is bounded.
        sessions_.erase(session_key);
        while (sessions_.size() >= policy_.max_sessions) {
            auto oldest = std::min_element(sessions_.begin(), sessions_.end(),
                [](const auto& a, const auto& b) {
                    return a.second->access_order < b.second->access_order;
                });
            sessions_.erase(oldest);
        }
        auto fresh = std::make_unique<SessionData>();
        fresh->kv_cache = std::make_unique<gcpp::KVCache>(
            gemma_->Config(), gemma_->Inference(), ctx_->allocator);
        validateBudget(0, tokens.size(), max_tokens, fresh->kv_cache->SeqLen());
        session = fresh.get();
        sessions_.emplace(session_key, std::move(fresh));
    }
    size_t streamed = 0;
    std::string answer;
    gcpp::RuntimeConfig runtime;
    gemma_->Inference().CopyTo(runtime);
    runtime.max_generated_tokens = max_tokens;
    runtime.temperature = temperature;
    runtime.verbosity = 0;
    runtime.stream_token = [&](int token, float) {
        ++streamed;
        if (streamed <= tokens.size() || gemma_->Config().IsEOS(token)) return true;
        std::string text;
        if (gemma_->Tokenizer().Decode({token}, &text)) {
            answer += text;
            if (on_token) on_token(text);
        }
        return true;
    };
    gcpp::TimingInfo timing;
    try {
        gemma_->Generate(runtime, gcpp::PromptTokens(tokens.data(), tokens.size()),
                         start_pos, 0, *session->kv_cache, *matmul_env_, timing);
        session->abs_pos = start_pos + streamed;
        session->transcript.clear();
        if (history) {
            for (const auto& message : *history) session->transcript.push_back(message.first);
            session->transcript.push_back(std::move(answer));
        }
        session->last_used = Clock::now();
        session->access_order = ++access_order_;
    } catch (...) {
        // A thrown callback/engine failure leaves partially mutated KV state.
        sessions_.erase(session_key);
        throw;
    }
}

void GemmaInferencePool::generate(
    const std::string& session_key, const std::string& prompt,
    std::function<void(const std::string&)> on_token,
    float temperature, size_t max_tokens) {
    generateImpl(session_key, prompt, nullptr, std::move(on_token), temperature, max_tokens);
}
std::string GemmaInferencePool::generateSync(
    const std::string& session_key, const std::string& prompt,
    float temperature, size_t max_tokens) {
    std::string answer;
    generate(session_key, prompt, [&](const std::string& token) { answer += token; },
             temperature, max_tokens);
    return answer;
}
void GemmaInferencePool::generateFromHistory(
    const std::string& session_key, const History& history,
    std::function<void(const std::string&)> on_token,
    float temperature, size_t max_tokens) {
    generateImpl(session_key, history.empty() ? std::string{} : history.back().first,
                 &history, std::move(on_token), temperature, max_tokens);
}
std::string GemmaInferencePool::generateSyncFromHistory(
    const std::string& session_key, const History& history,
    float temperature, size_t max_tokens) {
    std::string answer;
    generateFromHistory(session_key, history,
                        [&](const std::string& token) { answer += token; },
                        temperature, max_tokens);
    return answer;
}
