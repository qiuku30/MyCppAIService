#include"../include/AIUtil/AIStrategy.h"
#include"../include/AIUtil/AIFactory.h"
#include <algorithm>
#include <cstdlib>
#include <stdexcept>

// ========== AIStrategy 默认流式接口实现 ==========

json AIStrategy::buildStreamRequest(
    const std::vector<std::pair<std::string, long long>>& messages) const {
    json req = buildRequest(messages);
    req["stream"] = true;
    return req;
}

bool AIStrategy::parseStreamChunk(const std::string& sseData,
                                   std::string& tokenText) const {
    if (sseData.empty() || sseData == "[DONE]") return false;
    try {
        auto j = json::parse(sseData);
        if (j.contains("choices") && !j["choices"].empty() &&
            j["choices"][0].contains("delta") &&
            j["choices"][0]["delta"].contains("content")) {
            tokenText = j["choices"][0]["delta"]["content"];
            return !tokenText.empty();
        }
    } catch (...) {}
    return false;
}

// ========== 云端策略实现 ==========

std::string AliyunStrategy::getApiUrl() const {
    return "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
}

std::string AliyunStrategy::getApiKey()const {
    return apiKey_;
}


std::string AliyunStrategy::getModel() const {
    return "qwen-plus";
}


json AliyunStrategy::buildRequest(const std::vector<std::pair<std::string, long long>>& messages) const {
    json payload;
    payload["model"] = getModel();
    json msgArray = json::array();

    for (size_t i = 0; i < messages.size(); ++i) {
        json msg;
        if (i % 2 == 0) {
            msg["role"] = "user";
        }
        else {
            msg["role"] = "assistant";
        }
        msg["content"] = messages[i].first;
        msgArray.push_back(msg);
    }
    payload["messages"] = msgArray;
    return payload;
}


std::string AliyunStrategy::parseResponse(const json& response) const {
    if (response.contains("choices") && !response["choices"].empty()) {
        return response["choices"][0]["message"]["content"];
    }
    return {};
}


std::string DouBaoStrategy::getApiUrl()const {
    return "https://ark.cn-beijing.volces.com/api/v3/chat/completions";
}

std::string DouBaoStrategy::getApiKey()const {
    return apiKey_;
}


std::string DouBaoStrategy::getModel() const {
    return "doubao-seed-1-6-thinking-250715";
}


json DouBaoStrategy::buildRequest(const std::vector<std::pair<std::string, long long>>& messages) const {
    json payload;
    payload["model"] = getModel();
    json msgArray = json::array();

    for (size_t i = 0; i < messages.size(); ++i) {
        json msg;
        if (i % 2 == 0) {
            msg["role"] = "user";
        }
        else {
            msg["role"] = "assistant";
        }
        msg["content"] = messages[i].first;
        msgArray.push_back(msg);
    }
    payload["messages"] = msgArray;
    return payload;
}


std::string DouBaoStrategy::parseResponse(const json& response) const {
    if (response.contains("choices") && !response["choices"].empty()) {
        return response["choices"][0]["message"]["content"];
    }
    return {};
}


std::string AliyunRAGStrategy::getApiUrl() const {
    const char* value = std::getenv("KNOWLEDGE_BASE_ID");
    if (!value) value = std::getenv("Knowledge_Base_ID"); // Legacy configuration.
    const std::string id = value ? value : "";
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_';
        })) throw std::runtime_error("KNOWLEDGE_BASE_ID must contain the DashScope application ID");
    return "https://dashscope.aliyuncs.com/api/v1/apps/" + id + "/completion";
}

std::string AliyunRAGStrategy::getApiKey()const {
    return apiKey_;
}


std::string AliyunRAGStrategy::getModel() const {
    return ""; //Ҫģ
}


json AliyunRAGStrategy::buildRequest(const std::vector<std::pair<std::string, long long>>& messages) const {
    json payload;
    json msgArray = json::array();
    for (size_t i = 0; i < messages.size(); ++i) {
        json msg;
        msg["role"] = (i % 2 == 0 ? "user" : "assistant");
        msg["content"] = messages[i].first;
        msgArray.push_back(msg);
    }
    payload["input"]["messages"] = msgArray;
    payload["parameters"] = json::object(); 
    return payload;
}


std::string AliyunRAGStrategy::parseResponse(const json& response) const {
    if (response.contains("output") && response["output"].contains("text")) {
        return response["output"]["text"];
    }
    return {};
}

json AliyunRAGStrategy::buildStreamRequest(
    const std::vector<std::pair<std::string, long long>>& messages) const {
    auto payload = buildRequest(messages);
    payload["parameters"]["incremental_output"] = true;
    return payload;
}

std::vector<std::string> AliyunRAGStrategy::streamHeaders() const {
    return {"X-DashScope-SSE: enable", "Accept: text/event-stream"};
}

bool AliyunRAGStrategy::parseStreamChunk(const std::string& event, std::string& text) const {
    text.clear();
    try {
        auto parsed = json::parse(event);
        if (!parsed.contains("output") || !parsed["output"].is_object()) return false;
        const auto& output = parsed["output"];
        if (output.contains("text") && output["text"].is_string()) {
            text = output["text"].get<std::string>();
            return !text.empty();
        }
    } catch (const json::exception&) {}
    return false;
}

bool AliyunRAGStrategy::isStreamComplete(const json& event) const {
    if (!event.contains("output")) return false;
    if (!event["output"].is_object())
        throw std::runtime_error("Invalid DashScope application output");
    const auto& output = event["output"];
    if (output.contains("text") && !output["text"].is_string())
        throw std::runtime_error("Invalid DashScope application text");
    if (!output.contains("finish_reason") || output["finish_reason"].is_null()) return false;
    const auto reason = output["finish_reason"].get<std::string>();
    if (reason.empty() || reason == "null") return false;
    if (reason == "stop") return true;
    throw std::runtime_error("DashScope application stream did not finish normally");
}




std::string AliyunMcpStrategy::getApiUrl() const {
    return "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
}

std::string AliyunMcpStrategy::getApiKey()const {
    return apiKey_;
}


std::string AliyunMcpStrategy::getModel() const {
    return "qwen-plus";
}


json AliyunMcpStrategy::buildRequest(const std::vector<std::pair<std::string, long long>>& messages) const {
    json payload;
    payload["model"] = getModel();
    json msgArray = json::array();

    for (size_t i = 0; i < messages.size(); ++i) {
        json msg;
        if (i % 2 == 0) {
            msg["role"] = "user";
        }
        else {
            msg["role"] = "assistant";
        }
        msg["content"] = messages[i].first;
        msgArray.push_back(msg);
    }
    payload["messages"] = msgArray;
    return payload;
}


std::string AliyunMcpStrategy::parseResponse(const json& response) const {
    if (response.contains("choices") && !response["choices"].empty()) {
        return response["choices"][0]["message"]["content"];
    }
    return {};
}


// ========== GemmaStrategy 本地推理 ==========

std::string GemmaStrategy::getApiUrl() const {
    return "local://gemma";
}

std::string GemmaStrategy::getApiKey() const {
    return "";
}

std::string GemmaStrategy::getModel() const {
    return "gemma-2-2b-it";
}

json GemmaStrategy::buildRequest(
    const std::vector<std::pair<std::string, long long>>& messages) const {
    json j;
    j["model"] = getModel();
    json msgArray = json::array();
    for (size_t i = 0; i < messages.size(); ++i) {
        json msg;
        msg["role"] = (i % 2 == 0) ? "user" : "assistant";
        msg["content"] = messages[i].first;
        msgArray.push_back(msg);
    }
    j["messages"] = msgArray;
    return j;
}

std::string GemmaStrategy::parseResponse(const json& response) const {
    return response.value("text", "");
}

// ========== 策略自动注册 ==========

static StrategyRegister<AliyunStrategy> regAliyun("1");
static StrategyRegister<DouBaoStrategy> regDoubao("2");
static StrategyRegister<AliyunRAGStrategy> regAliyunRag("3");
static StrategyRegister<AliyunMcpStrategy> regAliyunMcp("4");
static StrategyRegister<GemmaStrategy> regGemma("5");