/**
 * ===========================================================================
 * AIConfig.h — MCP工具调用协议的数据结构与配置管理
 * ===========================================================================
 *
 * 【协议总览】
 *   自研 MCP (Model Context Protocol) 是一个纯 Prompt Engineering 方案，
 *   让 LLM 能够动态判断何时调用外部工具，从而实现"AI 驱动第三方 API"的能力。
 *
 *   协议流程 (两阶段推理):
 *     Phase 1: 用户输入 → buildPrompt (注入工具列表) → LLM 决策 → parseAIResponse
 *     Phase 2: 执行工具 → buildToolResultPrompt (注入结果) → LLM 生成自然语言回答
 *
 *   架构分层:
 *     config.json → AIConfig (提示词层) → AIHelper (编排层) → AIToolRegistry (执行层)
 */

#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <regex>
#include <fstream>
#include <sstream>
#include <iostream>
#include "../../../../HttpServer/include/utils/JsonUtil.h"


// ============================================================================
// AITool — 单个工具的定义 (从 config.json 的 tools 数组加载)
// ============================================================================
// 示例 JSON:
//   {"name":"get_weather", "params":{"city":"北京"}, "desc":"获取天气"}
//
// 字段说明:
//   name   — 工具唯一标识名，LLM 返回的 tool 字段必须匹配此名称
//   params — 参数列表，key=参数名，value=示例值 (仅用于展示给 LLM)
//   desc   — 功能描述，帮助 LLM 理解何时应该调用此工具
//
// 【设计要点】 params 的 value 只是示例值，序列化给 LLM 时只展示 key
//             LLM 不需要知道默认值，它是根据上下文推理参数应该填什么
// ============================================================================
struct AITool {
    std::string name;                                // 工具名称，如 "get_weather"
    std::unordered_map<std::string, std::string> params;  // 参数定义，如 {"city": "北京"}
    std::string desc;                                // 功能描述，如 "获取天气"
};


// ============================================================================
// AIToolCall — parseAIResponse 的解析结果，表示 LLM 是否决定调用工具
// ============================================================================
// 这是协议中的关键数据结构，承载 LLM → 应用层的意图传递。
//
// LLM 必须输出如下格式的 JSON (由 prompt_template 指示):
//   {"tool":"get_weather", "args":{"city":"北京"}}
//
// 字段说明:
//   toolName   — LLM 选择调用的工具名
//   args       — LLM 推理出的调用参数 (JSON 对象)
//   isToolCall — false=普通回复 / true=需要执行工具
//
// 【设计要点】 isToolCall 是一个"安全开关":
//             LLM 输出不可靠，parseAIResponse 三层容错确保此标志正确设置
// ============================================================================
struct AIToolCall {
    std::string toolName;   // 工具名称，与 AITool::name 对应
    json args;              // 工具调用参数，JSON 对象格式
    bool isToolCall = false; // 默认 false → 未明确识别时不执行工具 (安全优先)
};


// ============================================================================
// AIConfig — MCP 协议的提示词与配置管理
// ============================================================================
// 职责:
//   1. 从 JSON 文件加载提示词模板和工具定义 (loadFromFile)
//   2. 运行时构建注入工具列表的提示词 (buildPrompt / buildToolResultPrompt)
//   3. 解析 LLM 返回结果 (parseAIResponse)
//
// 使用方式 (在 AIHelper::chat 中):
//   AIConfig config;
//   config.loadFromFile("config.json");
//   string prompt      = config.buildPrompt(userInput);        // Phase 1
//   AIToolCall call    = config.parseAIResponse(llmResponse);  // 解析决策
//   string resultPrompt = config.buildToolResultPrompt(...);    // Phase 2
// ============================================================================
class AIConfig {
public:
    // ── 配置加载 ─────────────────────────────────────────────────────────
    // 从 JSON 文件加载 prompt_template 和 tools 数组
    // 返回 false 表示文件不存在或格式有误
    bool loadFromFile(const std::string& path);

    // ── Phase 1: 第一次推理的提示词构建 ──────────────────────────────────
    // 将 {user_input} 和 {tool_list} 占位符替换为实际内容
    // 生成的提示词被临时 push 到消息栈，LLM 返回后立即 pop (不污染历史)
    std::string buildPrompt(const std::string& userInput) const;

    // ── 决策解析 ─────────────────────────────────────────────────────────
    // 分析 LLM 返回的字符串，判断是工具调用 JSON 还是普通文本回复
    // 三层容错: JSON合法+含tool字段 / JSON合法+不含tool字段 / 非JSON文本
    AIToolCall parseAIResponse(const std::string& response) const;

    // ── Phase 2: 第二次推理的提示词构建 ──────────────────────────────────
    // 工具执行完毕后，将工具结果 + 原始用户问题打包为新的提示词
    // 要求 LLM 基于工具返回的结构化数据，生成用户友好的自然语言回答
    std::string buildToolResultPrompt(
        const std::string& userInput,
        const std::string& toolName,
        const json& toolArgs,
        const json& toolResult) const;

private:
    std::string promptTemplate_;    // 从 config.json 加载的提示词模板 (含占位符)
    std::vector<AITool> tools_;    // 从 config.json 加载的工具定义列表

    // ── 内部辅助 ─────────────────────────────────────────────────────────
    // 将 tools_ 序列化为 LLM 可读的工具列表字符串
    // 格式: "toolName(param1, param2) → description\n"
    std::string buildToolList() const;
};
