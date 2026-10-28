#include"../include/AIUtil/AIConfig.h"

bool AIConfig::loadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[AIConfig] Unable to open configuration file: " << path << std::endl;
        return false;
    }

    json j;
    file >> j;

    if (!j.contains("prompt_template") || !j["prompt_template"].is_string()) {
        std::cerr << "[AIConfig] prompt_template is missing" << std::endl;
        return false;
    }
    promptTemplate_ = j["prompt_template"].get<std::string>();

    if (j.contains("tools") && j["tools"].is_array()) {
        for (auto& tool : j["tools"]) {
            AITool t;
            t.name = tool.value("name", "");
            t.desc = tool.value("desc", "");
            if (tool.contains("params") && tool["params"].is_object()) {
                for (auto& [key, val] : tool["params"].items()) {
                    t.params[key] = val.get<std::string>();
                }
            }
            tools_.push_back(std::move(t));
        }
    }
    return true;
}

std::string AIConfig::buildToolList() const {
    std::ostringstream oss;
    for (const auto& t : tools_) {
        oss << t.name << "(";
        bool first = true;
        for (const auto& [key, val] : t.params) {
            if (!first) oss << ", ";
            oss << key;  // 只输出参数名
            first = false;
        }
        oss << ") → " << t.desc << "\n";
    }
    return oss.str();
}

std::string AIConfig::buildPrompt(const std::string& userInput) const {
    std::string result = promptTemplate_;
    result = std::regex_replace(result, std::regex("\\{user_input\\}"), userInput);
    result = std::regex_replace(result, std::regex("\\{tool_list\\}"), buildToolList());
    return result;
}

AIToolCall AIConfig::parseAIResponse(const std::string& response) const {
    AIToolCall result;
    try {
        // 预处理：strip LLM 可能包裹的 markdown 代码块标记
        std::string cleaned = response;
        // 去除首尾空白
        size_t first = cleaned.find_first_not_of(" \t\n\r");
        size_t last  = cleaned.find_last_not_of(" \t\n\r");
        if (first != std::string::npos && last != std::string::npos) {
            cleaned = cleaned.substr(first, last - first + 1);
        }
        // 去掉可能的 markdown ```json ... ``` 包裹
        if (cleaned.size() >= 7 && cleaned.substr(0, 7) == "```json") {
            cleaned = cleaned.substr(7);
            if (cleaned.size() >= 3 && cleaned.substr(cleaned.size() - 3) == "```") {
                cleaned = cleaned.substr(0, cleaned.size() - 3);
            }
        } else if (cleaned.size() >= 3 && cleaned.substr(0, 3) == "```") {
            cleaned = cleaned.substr(3);
            if (cleaned.size() >= 3 && cleaned.substr(cleaned.size() - 3) == "```") {
                cleaned = cleaned.substr(0, cleaned.size() - 3);
            }
        }
        // 再次 trim
        first = cleaned.find_first_not_of(" \t\n\r");
        last  = cleaned.find_last_not_of(" \t\n\r");
        if (first != std::string::npos && last != std::string::npos) {
            cleaned = cleaned.substr(first, last - first + 1);
        }

        json j = json::parse(cleaned);
        if (j.contains("tool") && j["tool"].is_string()) {
            result.toolName = j["tool"].get<std::string>();
            if (j.contains("args") && j["args"].is_object()) {
                result.args = j["args"];
            }
            result.isToolCall = true;
        }
    }
    catch (...) {
        // LLM 输出不可靠，解析失败时回退为普通文本回复
        result.isToolCall = false;
    }
    return result;
}

std::string AIConfig::buildToolResultPrompt(
    const std::string& userInput,
    const std::string& toolName,
    const json& toolArgs,
    const json& toolResult) const
{
    std::ostringstream oss;
    oss << "下面是用户说的话：" << userInput << "\n"
        << "我刚才调用了工具 [" << toolName << "] ，参数为："
        << toolArgs.dump() << "\n"
        << "工具返回的结果如下：\n" << toolResult.dump(4) << "\n"
        << "请根据以上信息，用自然语言回答用户。";
    return oss.str();
}
