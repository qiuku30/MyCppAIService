#include"../include/AIUtil/AIHelper.h"
#include"../include/AIUtil/MQManager.h"
#include"../include/AIUtil/GemmaInferencePool.h"
#include"../include/AIUtil/ChatMessage.h"
#include"../include/AIUtil/SecureId.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include<chrono>
#include <muduo/base/Logging.h>

AIHelper::AIHelper() = default;

void AIHelper::setStrategy(std::shared_ptr<AIStrategy> strat) {
    strategy = strat;
}

void AIHelper::restoreMessage(const std::string& userInput, long long ms) {
    std::lock_guard<std::mutex> lock(chat_mutex_);
    messageRoles_.push_back(messages.size() % 2 == 0);
    try {
        messages.push_back({userInput, ms});
    } catch (...) {
        messageRoles_.pop_back();
        throw;
    }
}

void AIHelper::restoreMessage(const std::string& content, long long ms, bool isUser) {
    std::lock_guard<std::mutex> lock(chat_mutex_);
    messageRoles_.push_back(isUser);
    try {
        messages.push_back({content, ms});
    } catch (...) {
        messageRoles_.pop_back();
        throw;
    }
}

void AIHelper::validateCanonicalHistory() const {
    if (messages.size() != messageRoles_.size())
        throw std::logic_error("Inconsistent stored chat history metadata");
    if (messages.size() % 2 != 0)
        throw std::runtime_error("Stored chat history has an incomplete exchange; start a new session to continue");
    for (size_t i = 0; i < messageRoles_.size(); ++i) {
        if (messageRoles_[i] != (i % 2 == 0))
            throw std::runtime_error("Stored chat history has incompatible roles; start a new session to continue");
    }
}

std::string AIHelper::chat(int userId, std::string userName, std::string sessionId,
                           std::string userQuestion, std::string modelType) {
    std::lock_guard<std::mutex> lock(chat_mutex_);
    validateCanonicalHistory();
    setStrategy(StrategyFactory::instance().create(modelType));

    // Upstream requests are temporary snapshots. Commit only completed exchanges.
    auto requestMessages = messages;
    requestMessages.push_back({userQuestion, 0});
    std::string answer;
    if (strategy->isLocal()) {
        if (!GemmaInferencePool::instance().isLoaded())
            throw std::runtime_error("Gemma model not loaded");
        answer = GemmaInferencePool::instance().generateSyncFromHistory(
            std::to_string(userId) + ":" + sessionId, requestMessages);
    } else if (!strategy->isMCPModel) {
        answer = strategy->parseResponse(executeCurl(strategy->buildRequest(requestMessages)));
    } else {
        AIConfig config;
        if (!config.loadFromFile("../AIApps/ChatServer/resource/config.json"))
            throw std::runtime_error("Tool configuration unavailable");
        requestMessages.back().first = config.buildPrompt(userQuestion);
        auto aiResult = strategy->parseResponse(executeCurl(strategy->buildRequest(requestMessages)));
        auto call = config.parseAIResponse(aiResult);
        if (!call.isToolCall) {
            answer = aiResult;
        } else {
            AIToolRegistry registry;
            auto toolResult = registry.invoke(call.toolName, call.args);
            requestMessages.back().first = config.buildToolResultPrompt(
                userQuestion, call.toolName, call.args, toolResult);
            answer = strategy->parseResponse(executeCurl(strategy->buildRequest(requestMessages)));
        }
    }
    if (answer.empty()) throw std::runtime_error("Empty model response");
    commitExchange(userId, userName, sessionId, userQuestion, answer);
    return answer;
}

void AIHelper::chatStream(int userId, std::string userName, std::string sessionId,
                          std::string userQuestion, std::string modelType,
                          std::function<void(const std::string&)> on_token,
                          std::function<void(const std::string&)> on_done,
                          std::function<void(const std::string&)> on_error) {
    try {
        std::unique_lock<std::mutex> lock(chat_mutex_);
        validateCanonicalHistory();
        setStrategy(StrategyFactory::instance().create(modelType));
        if (strategy->isMCPModel) {
            lock.unlock();
            auto answer = chat(userId, userName, sessionId, userQuestion, modelType);
            if (on_token) on_token(answer);
            if (on_done) on_done(answer);
            return;
        }

        auto requestMessages = messages;
        requestMessages.push_back({userQuestion, 0});
        std::string full_text;
        auto chunk = [&](const std::string& delta) {
            full_text += delta;
            if (on_token) on_token(delta);
        };
        if (strategy->isLocal()) {
            if (!GemmaInferencePool::instance().isLoaded())
                throw std::runtime_error("Gemma model not loaded");
            GemmaInferencePool::instance().generateFromHistory(
                std::to_string(userId) + ":" + sessionId, requestMessages, chunk);
        } else {
            bool failed = false;
            std::string failure;
            executeCurlStream(strategy->buildStreamRequest(requestMessages), chunk, nullptr,
                [&](const std::string& error) { failed = true; failure = error; });
            if (failed) throw std::runtime_error(failure);
        }
        if (full_text.empty()) throw std::runtime_error("Empty model response");
        commitExchange(userId, userName, sessionId, userQuestion, full_text);
        if (on_done) on_done(full_text);
    } catch (const std::exception& e) {
        if (on_error) on_error(e.what());
    }
}

json AIHelper::request(const json& payload) {
    std::lock_guard<std::mutex> lock(chat_mutex_);
    if (!strategy) strategy = StrategyFactory::instance().create("1");
    return executeCurl(payload);
}

std::vector<std::pair<std::string, long long>> AIHelper::GetMessages() {
    std::lock_guard<std::mutex> lock(chat_mutex_);
    return this->messages;
}

std::vector<AIHelper::HistoryMessage> AIHelper::GetHistorySnapshot() {
    std::lock_guard<std::mutex> lock(chat_mutex_);
    if (messages.size() != messageRoles_.size())
        throw std::logic_error("Inconsistent stored chat history metadata");
    std::vector<HistoryMessage> snapshot;
    snapshot.reserve(messages.size());
    for (size_t i = 0; i < messages.size(); ++i)
        snapshot.push_back({messages[i].first, messages[i].second, messageRoles_[i]});
    return snapshot;
}

json AIHelper::executeCurl(const json& payload) {
    const auto url = strategy->getApiUrl();
    const auto payloadStr = payload.dump();
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize curl");
    }


    std::string readBuffer;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curlOwner(curl, &curl_easy_cleanup);
    struct curl_slist* headers = nullptr;
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headersOwner(nullptr, &curl_slist_free_all);
    auto appendHeader = [&](const std::string& value) {
        auto* next = curl_slist_append(headers, value.c_str());
        if (!next) throw std::runtime_error("Failed to allocate curl request headers");
        headersOwner.release();
        headers = next;
        headersOwner.reset(headers);
    };

    std::string authHeader = "Authorization: Bearer " + strategy->getApiKey();
    appendHeader(authHeader);
    appendHeader("Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payloadStr.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    headersOwner.reset();
    curlOwner.reset();

    if (res != CURLE_OK) {
        throw std::runtime_error("curl_easy_perform() failed: " + std::string(curl_easy_strerror(res)));
    }

    if (status < 200 || status >= 300)
        throw std::runtime_error("Upstream HTTP error: " + std::to_string(status));
    try {
        auto response = json::parse(readBuffer);
        if (response.contains("error") || (response.contains("code") &&
            !response["code"].is_null() && response["code"] != "" &&
            response["code"] != "200" && response["code"] != 200))
            throw std::runtime_error("Upstream model reported an error");
        return response;
    } catch (const json::exception&) {
        throw std::runtime_error("Failed to parse upstream JSON response");
    }
}

// SSE 流式 curl — 在 curl 写回调中实时解析 SSE，立刻回调 on_chunk
void AIHelper::executeCurlStream(
    const json& payload, std::function<void(const std::string&)> on_chunk,
    std::function<void()> on_done, std::function<void(const std::string&)> on_error) {
    const auto url = strategy->getApiUrl();
    const auto extraHeaders = strategy->streamHeaders();
    const auto data = payload.dump();
    CURL* curl = curl_easy_init();
    if (!curl) { if (on_error) on_error("Failed to initialize curl"); return; }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curlOwner(curl, &curl_easy_cleanup);
    curl_slist* headers = nullptr;
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headersOwner(nullptr, &curl_slist_free_all);
    auto appendHeader = [&](const std::string& value) {
        auto* next = curl_slist_append(headers, value.c_str());
        if (!next) throw std::runtime_error("Failed to allocate curl request headers");
        headersOwner.release();
        headers = next;
        headersOwner.reset(headers);
    };
    auto auth = "Authorization: Bearer " + strategy->getApiKey();
    appendHeader(auth);
    appendHeader("Content-Type: application/json");
    for (const auto& header : extraHeaders) appendHeader(header);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
    struct State {
        std::string buffer, error;
        std::shared_ptr<AIStrategy> strategy;
        std::function<void(const std::string&)> chunk;
        bool completed = false;
    } state{{}, {}, strategy, on_chunk, false};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](void* contents, size_t size, size_t nmemb, void* context) -> size_t {
            auto& state = *static_cast<State*>(context);
            const size_t total = size * nmemb;
            try {
                state.buffer.append(static_cast<char*>(contents), total);
                size_t pos;
                while ((pos = state.buffer.find('\n')) != std::string::npos) {
                    auto line = state.buffer.substr(0, pos);
                    state.buffer.erase(0, pos + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.compare(0, 5, "data:") != 0) continue;
                    auto event = line.substr(5);
                    if (!event.empty() && event.front() == ' ') event.erase(0, 1);
                    if (event == "[DONE]") { state.completed = true; continue; }
                    if (event.empty()) continue;
                    auto parsed = json::parse(event);
                    if (parsed.contains("error") || (parsed.contains("code") &&
                        !parsed["code"].is_null() && parsed["code"] != "" &&
                        parsed["code"] != "200" && parsed["code"] != 200)) {
                        state.error = "Upstream model reported an error";
                        return 0;
                    }
                    if (state.completed) continue;
                    std::string token;
                    if (state.strategy->parseStreamChunk(event, token) && state.chunk)
                        state.chunk(token);
                    if (state.strategy->isStreamComplete(parsed)) state.completed = true;
                }
            } catch (const std::exception&) {
                state.error = "Invalid or failed stream callback";
                return 0;
            }
            return total;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    auto result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    headersOwner.reset();
    curlOwner.reset();
    if (!state.error.empty()) {
        if (on_error) on_error(state.error);
    } else if (result != CURLE_OK) {
        if (on_error) on_error(std::string("curl error: ") + curl_easy_strerror(result));
    } else if (status < 200 || status >= 300) {
        if (on_error) on_error("Upstream HTTP error: " + std::to_string(status));
    } else if (!state.completed) {
        if (on_error) on_error("Model stream ended before completion");
    } else {
        if (on_done) on_done();
    }
}

size_t AIHelper::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::string* buffer = static_cast<std::string*>(userp);
    buffer->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

void AIHelper::commitExchange(int userId, const std::string& userName,
    const std::string& sessionId, const std::string& question, const std::string& answer) {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (!messages.empty() && messages.back().second >= std::numeric_limits<long long>::max() - 2)
        throw std::overflow_error("Invalid restored chat timestamp");
    long long ts = messages.empty() ? now : std::max<long long>(now, messages.back().second + 1);
    ChatExchange exchange{secureId(), userId, userName, sessionId, question, answer, ts};
    auto body = exchange.serialize();
    ChatExchange::parse(body); // Validate bounds before queue admission.
    auto updated = messages;
    updated.push_back({question, ts});
    updated.push_back({answer, ts + 1});
    auto updatedRoles = messageRoles_;
    updatedRoles.push_back(true);
    updatedRoles.push_back(false);
    if (!MQManager::instance().publish("sql_queue", body))
        throw std::runtime_error("Chat history queue unavailable or full; exchange not accepted");
    messages.swap(updated);
    messageRoles_.swap(updatedRoles);
}
