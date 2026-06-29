#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <regex>
#include <fstream>
#include <sstream>
#include <iostream>
#include "../../../../HttpServer/include/utils/JsonUtil.h"

// 工具定义，从 config.json 的 tools 数组加载
struct AITool {
    std::string name;
    std::unordered_map<std::string, std::string> params;
    std::string desc;
};

// LLM 返回的工具调用解析结果
// LLM 输出格式: {"tool":"get_weather", "args":{"city":"北京"}}
struct AIToolCall {
    std::string toolName;
    json args;
    bool isToolCall = false;  // 默认 false，安全优先
};

// MCP 协议提示词管理与 LLM 响应解析
// 两阶段推理: buildPrompt → LLM决策 → parseAIResponse → 执行工具 → buildToolResultPrompt → LLM生成回答
class AIConfig {
public:
    bool loadFromFile(const std::string& path);

    // Phase 1: 构建注入工具列表的提示词
    std::string buildPrompt(const std::string& userInput) const;

    // 解析 LLM 返回，区分工具调用 JSON 和普通文本回复
    AIToolCall parseAIResponse(const std::string& response) const;

    // Phase 2: 将工具执行结果打包为二次推理提示词
    std::string buildToolResultPrompt(
        const std::string& userInput,
        const std::string& toolName,
        const json& toolArgs,
        const json& toolResult) const;

private:
    std::string promptTemplate_;
    std::vector<AITool> tools_;

    // 序列化工具列表为 LLM 可读字符串
    std::string buildToolList() const;
};
