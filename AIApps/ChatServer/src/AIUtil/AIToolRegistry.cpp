/**
 * ===========================================================================
 * AIToolRegistry.cpp — MCP工具注册表实现 (工具执行层)
 * ===========================================================================
 *
 * 【架构定位】
 *   AIConfig:      "LLM 想调用什么工具?" (意图识别层)
 *   AIToolRegistry: "实际执行这个工具"      (工具执行层)
 *   AIHelper:      "如何编排这个过程"      (流程编排层)
 *
 * 【统一接口契约】
 *   所有工具函数遵守: json → json
 *   这样的设计使得:
 *   - 新增工具不影响任何已有代码 (开闭原则)
 *   - LLM 返回的工具名直接映射到 C++ 函数 (字符串 → 函数指针)
 *   - 调用方 AIHelper 不需要知道工具的具体实现
 *
 * 【当前内置工具】
 *   get_weather(city) → 调用 wttr.in API 获取实时天气
 *   get_time()       → 返回系统时钟的当前时间
 */

#include "../include/AIUtil/AIToolRegistry.h"
#include <sstream>


// ============================================================================
// 构造函数 — 自动注册所有内置工具
// ============================================================================
// 【扩展指南】添加新工具只需在此处加一行:
//   registerTool("tool_name", toolFunction);
//
// 同时需要在 config.json 的 tools 数组中添加对应定义:
//   {"name":"tool_name", "params":{...}, "desc":"工具描述"}
//
// 这样 LLM 就能在提示词中看到新工具,并在合适的时机调用它
// ============================================================================
AIToolRegistry::AIToolRegistry() {
    registerTool("get_weather", getWeather);
    registerTool("get_time", getTime);
}


// ============================================================================
// registerTool — 将工具函数注册到注册表中
// ============================================================================
// O(1) 平均时间, 基于 unordered_map 的哈希查找
// 如果重复注册同名工具, 后者覆盖前者 (符合 unordered_map 语义)
// ============================================================================
void AIToolRegistry::registerTool(const std::string& name, ToolFunc func) {
    tools_[name] = func;
}


// ============================================================================
// invoke — 通过工具名执行工具调用
// ============================================================================
// 这是 MCP 协议执行阶段的核心方法。
//
// 调用链路:
//   AIHelper::chat() → parseAIResponse → AIToolCall{name, args}
//                    → registry.invoke(name, args) → json result
//                    → buildToolResultPrompt → 二次 LLM 调用
//
// 异常处理:
//   如果工具未注册 → throw runtime_error
//   调用方 AIHelper 通过 try-catch 捕获, 返回错误信息给前端
// ============================================================================
json AIToolRegistry::invoke(const std::string& name, const json& args) const {
    auto it = tools_.find(name);          // O(1) 哈希查找
    if (it == tools_.end()) {
        // 工具未注册: 可能是 LLM 幻觉出一个不存在的工具名
        throw std::runtime_error("Tool not found: " + name);
    }
    return it->second(args);              // 直接调用 C++ 函数, 传入 LLM 推理出的参数
}


// ============================================================================
// hasTool — 防御性检查工具是否存在
// ============================================================================
bool AIToolRegistry::hasTool(const std::string& name) const {
    return tools_.count(name) > 0;
}


// ============================================================================
// WriteCallback — libcurl 响应数据写入回调 (static 函数)
// ============================================================================
// 每次 curl 接收到数据块时被调用, 将数据追加到 output 字符串末尾
// 参数说明:
//   contents — 接收到的数据指针
//   size     — 每个数据单元的大小 (通常为 1)
//   nmemb    — 数据单元的数量
//   output   — 用户自定义指针 (CURLOPT_WRITEDATA 设置)
// 返回值: 成功处理的字节数 (必须等于 size*nmemb, 否则 curl 认为出错)
// ============================================================================
size_t AIToolRegistry::WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t totalSize = size * nmemb;
    output->append((char*)contents, totalSize);
    return totalSize;
}


// ============================================================================
// getWeather — 天气查询工具 (MCP 工具 #1)
// ============================================================================
// 功能: 通过 wttr.in 免费 API 查询指定城市的实时天气
//
// 【为什么选择 wttr.in 而不是专业天气 API?】
//   - 免费且无需注册 API Key
//   - 支持中文 (lang=zh)
//   - 返回格式简洁 (format=3 → 单行 "城市: 天气状况 温度")
//   - 纯 GET 请求，curl 3行代码搞定
//
// 输入格式: {"city": "北京"}
// 输出格式: {"city": "北京", "weather": "北京: 晴 15°C"}
//
// 错误处理:
//   - 缺少 city 参数 → {"error": "Missing parameter: city"}
//   - URL 编码失败   → {"error": "URL encode failed"}
//   - 网络请求失败   → {"error": "CURL request failed"}
//
// 【注意】工具函数的错误也返回 json (而非抛异常),
//         这样 LLM 能看到错误信息并在二次推理中向用户解释
// ============================================================================
json AIToolRegistry::getWeather(const json& args) {
    // ── 参数校验 ─────────────────────────────────────────────────────────
    if (!args.contains("city")) {
        return json{ {"error", "Missing parameter: city"} };
    }

    std::string city = args["city"].get<std::string>();
    std::string encodedCity;

    // ── URL 编码城市名 ───────────────────────────────────────────────────
    // 中文城市名 (如 "北京") 必须 URL 编码后才能放入 HTTP URL
    // curl_easy_escape 将 UTF-8 字节序列转换为 %XX 格式
    char* encoded = curl_easy_escape(nullptr, city.c_str(), city.length());
    if (encoded) {
        encodedCity = encoded;
        curl_free(encoded);             // ← 必须手动释放 curl 分配的内存
    }
    else {
        return json{ {"error", "URL encode failed"} };
    }

    // ── 构造请求 URL ─────────────────────────────────────────────────────
    // wttr.in API 格式: https://wttr.in/{city}?format=3&lang=zh
    // format=3 → 单行简洁格式
    // lang=zh  → 中文输出
    std::string url = "https://wttr.in/" + encodedCity + "?format=3&lang=zh";

    // ── curl HTTP GET 请求 ───────────────────────────────────────────────
    CURL* curl = curl_easy_init();
    std::string response;

    if (!curl) {
        return json{ {"error", "Failed to init CURL"} };
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);  // 响应数据写入 response
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);                   // 5 秒超时, 防止 LLM 等待过久
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);            // 跟随 HTTP 重定向

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);            // ← 必须清理, 否则内存泄漏

    if (res != CURLE_OK) {
        return json{ {"error", "CURL request failed"} };
    }

    // ── 返回结构化结果 ───────────────────────────────────────────────────
    // 返回 JSON 而非纯文本, 方便 LLM 解析并生成自然语言回答
    return json{ {"city", city}, {"weather", response} };
}


// ============================================================================
// getTime — 时间查询工具 (MCP 工具 #2)
// ============================================================================
// 功能: 获取系统当前时间
//
// 输入格式: {} (无需参数, args 被忽略)
// 输出格式: {"time": "2025-06-17 14:30:00"}
//
// 【设计对比】
//   getWeather: 外部 API 调用 (有网络延迟和失败风险)
//   getTime:    纯本地计算 (即时返回, 100% 可靠)
//   两种模式展示了 ToolRegistry 可以统一管理本地和远程工具
// ============================================================================
json AIToolRegistry::getTime(const json& args) {
    (void)args;                         // 显式忽略参数 (不需要但保留接口统一性)
    std::time_t t = std::time(nullptr); // Unix 时间戳 (秒)
    std::tm* now = std::localtime(&t);  // 转换为本地时间结构
    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", now);
    return json{ {"time", buffer} };
}
