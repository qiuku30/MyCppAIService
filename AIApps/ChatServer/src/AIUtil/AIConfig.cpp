/**
 * ===========================================================================
 * AIConfig.cpp — MCP工具调用协议的配置与提示词管理层
 * ===========================================================================
 *
 * 【本文件在系统中的定位】
 *   这是自研 MCP (Model Context Protocol) 工具调用协议的核心模块之一。
 *   负责三大职责:
 *     1. 从 config.json 加载提示词模板和工具定义 (loadFromFile)
 *     2. 构建发送给 LLM 的提示词 (buildPrompt / buildToolResultPrompt)
 *     3. 解析 LLM 返回结果，判断是工具调用还是普通文本 (parseAIResponse)
 *
 * 【为什么自研而不是用 OpenAI Function Calling?】
 *   - API 无关性: 纯 Prompt Engineering 方案，任何 Chat Completions API 都可用
 *   - 可控性: 提示词和解析逻辑完全自主，不依赖黑盒 API
 *   - C++ 原生: 无需 Python SDK，零外部依赖
 *
 * 【数据流路径】
 *   config.json → loadFromFile() → promptTemplate_ + tools_
 *        ↓
 *   AIHelper::chat() 调用 buildPrompt(userInput) → 注入工具列表的完整提示词
 *        ↓
 *   第一次 LLM 调用 → parseAIResponse(response) → AIToolCall
 *        ↓
 *   如果 isToolCall=true → 执行工具 → buildToolResultPrompt() → 第二次 LLM 调用
 */

#include"../include/AIUtil/AIConfig.h"

// ============================================================================
// loadFromFile — 从 JSON 配置文件加载提示词模板和工具定义
// ============================================================================
// 配置文件格式 (config.json):
//   {
//     "prompt_template": "你是一个...{user_input}...{tool_list}...",
//     "tools": [
//       {"name":"get_weather", "params":{"city":"北京"}, "desc":"获取天气"},
//       {"name":"get_time",    "params":{},          "desc":"获取当前时间"}
//     ]
//   }
//
// 【设计要点】
//   - 工具定义完全配置化: 新增工具只需修改 config.json，无需重新编译 C++ 代码
//   - 符合开闭原则: 对扩展开放(加新工具)，对修改封闭(不改 C++ 代码)
//   - prompt_template 中的 {user_input} 和 {tool_list} 是运行时替换的占位符
// ============================================================================
bool AIConfig::loadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[AIConfig] Unable to open configuration file: " << path << std::endl;
        return false;
    }

    // 使用 nlohmann/json 解析配置文件
    json j;
    file >> j;

    // ── 第一步: 加载提示词模板 ──────────────────────────────────────────
    // prompt_template 是必须字段，包含两个占位符:
    //   {user_input} → 运行时替换为用户的实际输入
    //   {tool_list}  → 运行时替换为可用工具列表
    // 如果缺失此字段，整个 MCP 流程无法工作，返回 false
    if (!j.contains("prompt_template") || !j["prompt_template"].is_string()) {
        std::cerr << "[AIConfig] prompt_template is missing" << std::endl;
        return false;
    }
    promptTemplate_ = j["prompt_template"].get<std::string>();

    // ── 第二步: 加载工具定义列表 ──────────────────────────────────────────
    // tools 数组是可选的: 即使没有工具，MCP 也能作为普通对话使用
    // 每个工具定义包含:
    //   name   — 工具名 (LLM 返回的 JSON 中的 "tool" 字段)
    //   params — 参数列表 (key=参数名, value=示例值)，序列化时只取 key
    //   desc   — 功能描述 (展示给 LLM 帮助其判断何时调用)
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
            tools_.push_back(std::move(t));  // move 语义避免拷贝
        }
    }
    return true;
}

// ============================================================================
// buildToolList — 将注册的工具列表序列化为 LLM 可读的文本格式
// ============================================================================
// 输出示例:
//   get_weather(city) → 获取天气
//   get_time() → 获取当前时间
//
// 【设计要点】
//   - 只输出参数名，不输出示例值: LLM 只需要知道参数签名即可推理
//   - 使用 → 符号分隔: 直观展示 "工具签名 → 功能描述" 的映射关系
//   - 此文本会被注入到 prompt_template 的 {tool_list} 占位符位置
// ============================================================================
std::string AIConfig::buildToolList() const {
    std::ostringstream oss;
    for (const auto& t : tools_) {
        oss << t.name << "(";
        bool first = true;
        for (const auto& [key, val] : t.params) {
            if (!first) oss << ", ";
            oss << key;           // 只输出参数名，不输出默认值
            first = false;
        }
        oss << ") → " << t.desc << "\n";
    }
    return oss.str();
}

// ============================================================================
// buildPrompt — 构建发送给 LLM 的第一次提示词 (MCP 流程的第一阶段)
// ============================================================================
// 此方法将 prompt_template 中的两个占位符替换为实际内容:
//   {user_input} → 用户原始输入 (如 "帮我查一下北京天气")
//   {tool_list}  → buildToolList() 的输出 (可用工具列表)
//
// 生成的完整提示词示例 (详见 config.json):
//   "你是一个第三方中间人...
//    如果调用工具只输出JSON...
//    用户输入: 帮我查一下北京天气
//    可用工具: get_weather(city) → 获取天气
//              get_time() → 获取当前时间
//    如果调用请输出: {\"tool\":\"...\", \"args\":{...}}"
//
// 【调用场景】
//   此方法在 AIHelper::chat() 的 MCP 分支中被调用
//   生成的提示词被临时 push 到消息栈中，LLM 返回后立即 pop (避免污染历史)
// ============================================================================
std::string AIConfig::buildPrompt(const std::string& userInput) const {
    std::string result = promptTemplate_;
    // 使用正则替换确保所有出现的占位符都被替换 (虽然通常只有一处)
    result = std::regex_replace(result, std::regex("\\{user_input\\}"), userInput);
    result = std::regex_replace(result, std::regex("\\{tool_list\\}"), buildToolList());
    return result;
}

// ============================================================================
// parseAIResponse — 解析 LLM 返回的文本，判断是否为工具调用
// ============================================================================
// 这是 MCP 协议中最关键的解析逻辑，处理三种情况:
//
//   情况A: LLM 返回合法 JSON 且包含 "tool" 字段
//          → 提取 toolName 和 args → isToolCall=true
//          例如: {"tool":"get_weather", "args":{"city":"北京"}}
//
//   情况B: LLM 返回合法 JSON 但不包含 "tool" 字段
//          → isToolCall=false (LLM 选择直接回答)
//          例如: {"reply": "今天天气不错"}
//
//   情况C: LLM 返回的不是合法 JSON (格式错误/多余内容)
//          → json::parse() 抛异常 → catch → isToolCall=false
//          鲁棒回退: 当作文本回复处理
//
// 【为什么需要 catch(...) ？】
//   LLM 的输出不可靠——可能多一个换行、多一句解释、JSON 格式有误。
//   如果此处崩溃，整个 HTTP 请求返回 500。
//   鲁棒回退确保系统在任何情况下都能优雅降级。
// ============================================================================
AIToolCall AIConfig::parseAIResponse(const std::string& response) const {
    AIToolCall result;  // 默认 isToolCall=false
    try {
        // 尝试将 LLM 返回的字符串解析为 JSON 对象
        json j = json::parse(response);

        // 检查 JSON 中是否包含 "tool" 字段
        // prompt_template 明确指示 LLM: 调用工具时输出 {"tool":"...", "args":{...}}
        if (j.contains("tool") && j["tool"].is_string()) {
            result.toolName = j["tool"].get<std::string>();
            if (j.contains("args") && j["args"].is_object()) {
                result.args = j["args"];
            }
            result.isToolCall = true;  // ← 标志位: 告诉 AIHelper 需要执行工具
        }
        // 情况B: JSON 中没有 "tool" 字段 → isToolCall 保持 false
    }
    catch (...) {
        // 情况C: 解析失败 → 不是合法的 JSON 格式
        // LLM 可能返回了纯文本或多行内容，安全回退到普通回复路径
        result.isToolCall = false;
    }
    return result;
}

// ============================================================================
// buildToolResultPrompt — 构建第二次 LLM 调用的提示词 (MCP 流程的第二阶段)
// ============================================================================
// 在工具执行完成后，构建新的提示词将工具结果注入 LLM 上下文。
//
// 生成的提示词示例:
//   下面是用户说的话：帮我查一下北京天气
//   我刚才调用了工具 [get_weather] ，参数为：{"city":"北京"}
//   工具返回的结果如下：
//   {
//       "city": "北京",
//       "weather": "北京: 晴 15°C"
//   }
//   请根据以上信息，用自然语言回答用户。
//
// 【设计要点】
//   - 中文提示词: 与 prompt_template 保持语言一致性 (中文模型的场景)
//   - toolResult.dump(4): 参数 4 表示缩进 4 空格，让 JSON 可读性更好
//   - "用自然语言回答": 明确要求 LLM 将结构化数据转换成用户友好的表述
//   - 此提示词同样在 LLM 返回后立即 pop，不污染对话历史
// ============================================================================
std::string AIConfig::buildToolResultPrompt(
    const std::string& userInput,      // 用户原始问题 (保留上下文)
    const std::string& toolName,       // 被调用的工具名
    const json& toolArgs,              // 调用参数 (JSON)
    const json& toolResult) const      // 工具返回结果 (JSON)
{
    std::ostringstream oss;
    oss << "下面是用户说的话：" << userInput << "\n"
        << "我刚才调用了工具 [" << toolName << "] ，参数为："
        << toolArgs.dump() << "\n"
        << "工具返回的结果如下：\n" << toolResult.dump(4) << "\n"
        << "请根据以上信息，用自然语言回答用户。";
    return oss.str();
}
