#include"../include/AIUtil/AIHelper.h"
#include"../include/AIUtil/MQManager.h"
#include"../include/AIUtil/GemmaInferencePool.h"
#include <stdexcept>
#include<chrono>
#include <muduo/base/Logging.h>

AIHelper::AIHelper() {
    strategy = StrategyFactory::instance().create("1");
}

void AIHelper::setStrategy(std::shared_ptr<AIStrategy> strat) {
    strategy = strat;
}

void AIHelper::addMessage(int userId, const std::string& userName, bool is_user,
                          const std::string& userInput, std::string sessionId) {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    messages.push_back({userInput, ms});
    // 异步入库：通过 RabbitMQ 解耦数据库写入
    pushMessageToMysql(userId, userName, is_user, userInput, ms, sessionId);
}

void AIHelper::restoreMessage(const std::string& userInput, long long ms) {
    messages.push_back({userInput, ms});
}

std::string AIHelper::chat(int userId, std::string userName, std::string sessionId,
                           std::string userQuestion, std::string modelType) {
    std::lock_guard<std::mutex> lock(chat_mutex_);
    setStrategy(StrategyFactory::instance().create(modelType));

    // 本地模型：走 GemmaInferencePool
    if (strategy->isLocal()) {
        if (!GemmaInferencePool::instance().isLoaded()) {
            return "[Error] Gemma model not loaded";
        }
        addMessage(userId, userName, true, userQuestion, sessionId);
        std::string answer = GemmaInferencePool::instance().generateSync(
            sessionId, userQuestion);
        addMessage(userId, userName, false, answer, sessionId);
        return answer.empty() ? "[Error] Empty response" : answer;
    }

    // 非 MCP 模型：直接对话
    if (false == strategy->isMCPModel) {
        addMessage(userId, userName, true, userQuestion, sessionId);
        json payload = strategy->buildRequest(this->messages);
        json response = executeCurl(payload);
        std::string answer = strategy->parseResponse(response);
        addMessage(userId, userName, false, answer, sessionId);
        return answer.empty() ? "[Error] 无法解析响应" : answer;
    }

    // MCP 模式：两阶段推理
    // Phase 1: LLM 决策是否调用工具
    AIConfig config;
    config.loadFromFile("../AIApps/ChatServer/resource/config.json");
    std::string tempUserQuestion = config.buildPrompt(userQuestion);
    std::cout << "tempUserQuestion is " << tempUserQuestion << std::endl;

    ScopedMessage phase1Guard(messages, tempUserQuestion);  // RAII: 离开作用域自动 pop

    json firstReq = strategy->buildRequest(this->messages);
    json firstResp = executeCurl(firstReq);
    std::string aiResult = strategy->parseResponse(firstResp);

    std::cout << "aiResult is " << aiResult << std::endl;

    AIToolCall call = config.parseAIResponse(aiResult);

    // LLM 未选择工具 → 当作普通回复
    if (!call.isToolCall) {
        addMessage(userId, userName, true, userQuestion, sessionId);
        addMessage(userId, userName, false, aiResult, sessionId);
        std::cout << "No tools required" << std::endl;
        return aiResult;
    }

    // LLM 选择调用工具 → 执行 → Phase 2
    json toolResult;
    AIToolRegistry registry;

    try {
        toolResult = registry.invoke(call.toolName, call.args);
        std::cout << "Tool call success" << std::endl;
    } catch (const std::exception& e) {
        std::string err = "[工具调用失败] " + std::string(e.what());
        addMessage(userId, userName, true, userQuestion, sessionId);
        addMessage(userId, userName, false, err, sessionId);
        std::cout << "Tool call failed" << std::endl << std::string(e.what());
        return err;
    }

    // Phase 2: 注入工具结果，LLM 生成自然语言回答
    std::string secondPrompt = config.buildToolResultPrompt(
        userQuestion, call.toolName, call.args, toolResult);
    std::cout << "secondPrompt is " << secondPrompt << std::endl;

    ScopedMessage phase2Guard(messages, secondPrompt);  // RAII: 异常安全

    json secondReq = strategy->buildRequest(messages);
    json secondResp = executeCurl(secondReq);
    std::string finalAnswer = strategy->parseResponse(secondResp);

    std::cout << "finalAnswer is " << finalAnswer << std::endl;

    addMessage(userId, userName, true, userQuestion, sessionId);
    addMessage(userId, userName, false, finalAnswer, sessionId);
    return finalAnswer;
}

void AIHelper::chatStream(int userId, std::string userName, std::string sessionId,
                          std::string userQuestion, std::string modelType,
                          std::function<void(const std::string& token)> on_token,
                          std::function<void(const std::string& full_answer)> on_done,
                          std::function<void(const std::string& error)> on_error) {
    try {
        std::lock_guard<std::mutex> lock(chat_mutex_);
        setStrategy(StrategyFactory::instance().create(modelType));

        // === 路径 1: 本地 Gemma 推理 ===
        if (strategy->isLocal()) {
            if (!GemmaInferencePool::instance().isLoaded()) {
                on_error("Gemma model not loaded");
                return;
            }
            addMessage(userId, userName, true, userQuestion, sessionId);

            std::string full_text;
            GemmaInferencePool::instance().generate(
                sessionId, userQuestion,
                [&full_text, &on_token](const std::string& token) {
                    full_text += token;
                    if (on_token) on_token(token);
                });

            addMessage(userId, userName, false, full_text, sessionId);
            if (on_done) on_done(full_text);
            return;
        }

        // === 路径 2: 云端 API 非 MCP 流式 ===
        if (!strategy->isMCPModel) {
            addMessage(userId, userName, true, userQuestion, sessionId);
            json payload = strategy->buildStreamRequest(this->messages);

            std::string full_text;
            executeCurlStream(payload,
                // on_chunk
                [&full_text, &on_token](const std::string& delta) {
                    full_text += delta;
                    if (on_token) on_token(delta);
                },
                // on_done (stream ended)
                nullptr,
                // on_error
                [&on_error](const std::string& err) {
                    if (on_error) on_error(err);
                });

            addMessage(userId, userName, false, full_text, sessionId);
            if (on_done) on_done(full_text);
            return;
        }

        // === 路径 3: MCP 两阶段推理（暂保持非流式） ===
        AIConfig config;
        config.loadFromFile("../AIApps/ChatServer/resource/config.json");
        std::string tempUserQuestion = config.buildPrompt(userQuestion);

        ScopedMessage streamPhase1Guard(messages, tempUserQuestion);
        json firstReq = strategy->buildRequest(this->messages);
        json firstResp = executeCurl(firstReq);
        std::string aiResult = strategy->parseResponse(firstResp);

        AIToolCall call = config.parseAIResponse(aiResult);

        if (!call.isToolCall) {
            addMessage(userId, userName, true, userQuestion, sessionId);
            addMessage(userId, userName, false, aiResult, sessionId);
            if (on_done) on_done(aiResult);
            return;
        }

        json toolResult;
        AIToolRegistry registry;
        try {
            toolResult = registry.invoke(call.toolName, call.args);
        } catch (const std::exception& e) {
            std::string err = "[Tool call failed] " + std::string(e.what());
            addMessage(userId, userName, true, userQuestion, sessionId);
            addMessage(userId, userName, false, err, sessionId);
            if (on_done) on_done(err);
            return;
        }

        std::string secondPrompt = config.buildToolResultPrompt(
            userQuestion, call.toolName, call.args, toolResult);
        ScopedMessage streamPhase2Guard(messages, secondPrompt);
        json secondReq = strategy->buildRequest(messages);
        json secondResp = executeCurl(secondReq);
        std::string finalAnswer = strategy->parseResponse(secondResp);

        addMessage(userId, userName, true, userQuestion, sessionId);
        addMessage(userId, userName, false, finalAnswer, sessionId);
        if (on_done) on_done(finalAnswer);

    } catch (const std::exception& e) {
        if (on_error) on_error(e.what());
    }
}

json AIHelper::request(const json& payload) {
    return executeCurl(payload);
}

std::vector<std::pair<std::string, long long>> AIHelper::GetMessages() {
    return this->messages;
}

json AIHelper::executeCurl(const json& payload) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize curl");
    }

    std::cout << "test " << strategy->getApiUrl().c_str() << ' ' << strategy->getApiKey() << std::endl;

    std::string readBuffer;
    struct curl_slist* headers = nullptr;

    std::string authHeader = "Authorization: Bearer " + strategy->getApiKey();
    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string payloadStr = payload.dump();

    curl_easy_setopt(curl, CURLOPT_URL, strategy->getApiUrl().c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payloadStr.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error("curl_easy_perform() failed: " + std::string(curl_easy_strerror(res)));
    }

    try {
        return json::parse(readBuffer);
    } catch (...) {
        throw std::runtime_error("Failed to parse JSON response: " + readBuffer);
    }
}

// SSE 流式 curl — 在 curl 写回调中实时解析 SSE，立刻回调 on_chunk
void AIHelper::executeCurlStream(
    const json& payload,
    std::function<void(const std::string& delta)> on_chunk,
    std::function<void()> on_done,
    std::function<void(const std::string&)> on_error) {

    CURL* curl = curl_easy_init();
    if (!curl) {
        if (on_error) on_error("Failed to initialize curl");
        return;
    }

    struct curl_slist* headers = nullptr;
    std::string authHeader = "Authorization: Bearer " + strategy->getApiKey();
    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string payloadStr = payload.dump();

    curl_easy_setopt(curl, CURLOPT_URL, strategy->getApiUrl().c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payloadStr.c_str());

    // 流式解析状态：跨多次写回调增量解析 SSE
    struct StreamParseState {
        std::string buffer;      // 未完成的行数据
        std::shared_ptr<AIStrategy> strat;
        std::function<void(const std::string&)> on_chunk;
        std::function<void()> on_done;
    };
    StreamParseState state;
    state.strat = strategy;
    state.on_chunk = on_chunk;
    state.on_done = on_done;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
            size_t total = size * nmemb;
            auto* st = static_cast<StreamParseState*>(userp);
            st->buffer.append(static_cast<char*>(contents), total);

            // 按行增量解析 SSE（每行以 \n 结尾）
            size_t pos;
            while ((pos = st->buffer.find('\n')) != std::string::npos) {
                std::string line = st->buffer.substr(0, pos);
                st->buffer.erase(0, pos + 1);

                // 去掉行尾 \r
                if (!line.empty() && line.back() == '\r') line.pop_back();

                const std::string prefix = "data: ";
                if (line.compare(0, prefix.size(), prefix) != 0) continue;

                std::string data = line.substr(prefix.size());
                if (data == "[DONE]") {
                    if (st->on_done) st->on_done();
                    continue;
                }
                std::string token_text;
                if (st->strat->parseStreamChunk(data, token_text)) {
                    if (st->on_chunk) st->on_chunk(token_text);
                }
            }
            return total;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        if (on_error) on_error(std::string("curl error: ") + curl_easy_strerror(res));
        return;
    }
}

size_t AIHelper::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::string* buffer = static_cast<std::string*>(userp);
    buffer->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

std::string AIHelper::escapeString(const std::string& input) {
    std::string output;
    output.reserve(input.size() * 2);
    for (char c : input) {
        switch (c) {
            case '\\': output += "\\\\"; break;
            case '\'': output += "\\\'"; break;
            case '\"': output += "\\\""; break;
            case '\n': output += "\\n";  break;
            case '\r': output += "\\r";  break;
            case '\t': output += "\\t";  break;
            default:   output += c; break;
        }
    }
    return output;
}

void AIHelper::pushMessageToMysql(int userId, const std::string& userName, bool is_user,
                                  const std::string& userInput, long long ms, std::string sessionId) {
    std::string safeUserName = escapeString(userName);
    std::string safeUserInput = escapeString(userInput);

    std::string sql = "INSERT INTO chat_message (id, username, session_id, is_user, content, ts) VALUES ("
        + std::to_string(userId) + ", "
        + "'" + safeUserName + "', "
        + sessionId + ", "
        + std::to_string(is_user ? 1 : 0) + ", "
        + "'" + safeUserInput + "', "
        + std::to_string(ms) + ")";

    // 异步投递到 RabbitMQ，消费者线程执行 SQL
    // try-catch 保护：MQ 不可用时不影响对话主流程，消息已写入内存 messages 列表
    try {
        MQManager::instance().publish("sql_queue", sql);
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to publish message to MQ (chat continues in memory): " << e.what()
                  << " | sql=" << sql.substr(0, 200);
    }
}
