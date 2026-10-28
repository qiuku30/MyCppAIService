#include"../include/AIUtil/AIStrategy.h"
#include"../include/AIUtil/AIFactory.h"

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
    const char* key = std::getenv("Knowledge_Base_ID");
    if (!key) throw std::runtime_error("Knowledge_Base_ID not found!");
    std::string id(key);
    //϶Ӧ֪ʶID
    return "https://dashscope.aliyuncs.com/api/v1/apps/"+id+"/completion";
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