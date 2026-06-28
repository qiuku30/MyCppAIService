/**
 * ===========================================================================
 * AIToolRegistry.h — MCP工具注册表 (工具执行层)
 * ===========================================================================
 *
 * 【在 MCP 协议中的角色】
 *   AIConfig 负责 "LLM 想调用什么工具" (解析层)
 *   AIToolRegistry 负责 "实际执行这个工具" (执行层)
 *
 *   两者通过 ToolFunc 统一接口解耦:
 *     LLM 输出 → AIConfig::parseAIResponse → AIToolCall{toolName, args}
 *              → AIToolRegistry::invoke(toolName, args) → json result
 *              → 注入 LLM 上下文 → 二次推理
 *
 * 【ToolFunc 统一签名】
 *   json(const json&) — JSON进/JSON出，所有工具遵守同一契约:
 *     输入:  LLM 推理出的参数 {"city": "北京"}
 *     输出:  工具执行结果 {"city": "北京", "weather": "晴 15°C"}
 *
 * 【扩展方式】
 *   添加新工具只需两步, 不用修改任何协议代码:
 *     Step 1: config.json 的 tools 数组加一条定义
 *     Step 2: 构造函数加一行 registerTool("name", function)
 */

#pragma once
#include <string>
#include <unordered_map>
#include <functional>
#include <stdexcept>
#include <iostream>
#include <ctime>
#include <curl/curl.h>
#include "../../../../HttpServer/include/utils/JsonUtil.h"

class AIToolRegistry {
public:
    // ── ToolFunc: 工具函数的统一类型签名 ─────────────────────────────────
    // 所有工具函数必须遵守: 接受 json 参数, 返回 json 结果
    // 这是 MCP 协议中 "可调用工具" 的抽象契约
    // 类似 OpenAI Function Calling 中的 function 定义
    using ToolFunc = std::function<json(const json&)>;

    // ── 构造函数 ─────────────────────────────────────────────────────────
    // 自动注册所有内置工具 (get_weather, get_time)
    // 添加新工具时在此处增加 registerTool 调用即可
    AIToolRegistry();

    // ── registerTool: 注册一个新工具 ─────────────────────────────────────
    // name — 工具名，必须与 config.json 中的 name 和 LLM 返回的 tool 字段一致
    // func — 工具实现函数，签名必须为 json(const json&)
    void registerTool(const std::string& name, ToolFunc func);

    // ── invoke: 执行工具调用 ─────────────────────────────────────────────
    // 通过 AIConfig::parseAIResponse 解析出的 toolName 和 args
    // O(1) 查找 → 直接调用 C++ 函数 → 返回 json
    // 如果工具不存在则抛出 runtime_error (调用方 AIHelper 负责 catch)
    json invoke(const std::string& name, const json& args) const;

    // ── hasTool: 检查工具是否存在 ────────────────────────────────────────
    // 可用于调用前的防御性检查
    bool hasTool(const std::string& name) const;

private:
    // 工具注册表: name → function 映射，O(1) 查找
    std::unordered_map<std::string, ToolFunc> tools_;

    // ── 内置工具实现 ─────────────────────────────────────────────────────
    // curl 响应写入回调 (与 AIHelper 中的 WriteCallback 功能相同)
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output);

    // 天气查询工具: 调用 wttr.in 免费天气 API
    // 输入: {"city": "北京"}
    // 输出: {"city": "北京", "weather": "北京: 晴 15°C"}
    static json getWeather(const json& args);

    // 时间查询工具: 获取当前系统时间
    // 输入: {} (不需要参数)
    // 输出: {"time": "2025-06-17 14:30:00"}
    static json getTime(const json& args);
};
