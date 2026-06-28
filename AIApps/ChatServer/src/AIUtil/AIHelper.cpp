/**
 * ===========================================================================
 * AIHelper.cpp — AI交互核心引擎 (MCP流程编排 + 消息管理 + 异步持久化)
 * ===========================================================================
 *
 * 【AIHelper 在整个系统中的定位】
 *   AIHelper 是连接 ChatServer (业务层) 与 AI API (模型层) 的核心桥梁。
 *   每个 (userId, sessionId) 对对应一个 AIHelper 实例, 管理该会话的:
 *     - 对话历史 (messages 向量)
 *     - LLM 交互 (chat 方法)
 *     - 消息持久化 (pushMessageToMysql → RabbitMQ 异步写库)
 *
 *   ChatServer 通过 chatInformation[userId][sessionId] 存储和查找这些实例。
 *
 * 【chat() 方法的两种执行路径】
 *
 *   路径A — 普通对话 (isMCPModel=false):
 *     userInput → addMessage (持久化) → buildRequest → curl → parseResponse
 *     → addMessage (持久化) → return answer
 *     适用模型: 阿里百炼通义千问("1"), 豆包("2"), 阿里百炼RAG("3")
 *
 *   路径B — MCP工具调用 (isMCPModel=true):
 *     userInput → buildPrompt (注入工具列表)
 *     → Phase1: LLM 决策 → parseAIResponse (判断是否调用工具)
 *       → 不调用: 直接返回
 *       → 调用: invoke 工具 → buildToolResultPrompt (注入结果)
 *         → Phase2: LLM 生成自然语言回答
 *     → addMessage (持久化真实对话) → return answer
 *     适用模型: 阿里百炼MCP("4")
 *
 * 【异步持久化设计】
 *   addMessage() 分两步:
 *     1. 同步写入内存 (messages.push_back) — 保证即时可访问
 *     2. 异步写入 MySQL (MQManager::publish → RabbitMQ → 消费者线程执行 SQL)
 *   这种设计: AI API 调用耗时 2-10 秒期间, 数据库写入完全不阻塞主流程
 */

#include"../include/AIUtil/AIHelper.h"
#include"../include/AIUtil/MQManager.h"
#include <stdexcept>
#include<chrono>

// ============================================================================
// 构造函数 — 默认策略为阿里百炼通义千问 ("1")
// ============================================================================
// 策略可在运行时通过 chat() 的 modelType 参数动态切换
AIHelper::AIHelper() {
    //默认使用阿里云大模型
    strategy = StrategyFactory::instance().create("1");
}

// ============================================================================
// setStrategy — 运行时切换 AI 策略 (阿里百炼 / 豆包 / 阿里百炼RAG / 阿里百炼MCP)
// ============================================================================
void AIHelper::setStrategy(std::shared_ptr<AIStrategy> strat) {
    strategy = strat;
}


//设置默认模型
//void AIHelper::setModel(const std::string& modelName) {
  //  model_ = modelName;
//}

// ============================================================================
// addMessage — 添加一条消息到对话历史 (内存 + 异步持久化)
// ============================================================================
// 参数:
//   userId    — 用户ID (用于数据库存储)
//   userName  — 用户名 (用于数据库存储)
//   is_user   — true=用户消息, false=AI回复
//   userInput — 消息内容
//   sessionId — 会话ID (用于数据库按会话查询历史)
//
// 执行流程:
//   1. 生成毫秒级时间戳
//   2. 同步写入内存 (messages vector) → 当前请求立即可用
//   3. 异步入库 (MQManager → RabbitMQ → consumer → MySQL) → 不阻塞
// ============================================================================
void AIHelper::addMessage(int userId,const std::string& userName, bool is_user,const std::string& userInput, std::string sessionId) {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    messages.push_back({ userInput,ms });
    //消息队列异步入库 — 解耦 AI 响应与数据库写入, 实现削峰填谷
    pushMessageToMysql(userId, userName, is_user, userInput, ms, sessionId);
}

// ============================================================================
// restoreMessage — 从数据库恢复历史消息 (服务器启动时调用)
// ============================================================================
// 与 addMessage 的区别: 只恢复内存状态, 不重复写数据库
// 调用者: ChatServer::readDataFromMySQL() → 启动时重建所有会话的对话历史
void AIHelper::restoreMessage(const std::string& userInput,long long ms) {
    messages.push_back({ userInput,ms });
}


// ============================================================================
// chat — 核心 AI 交互方法 (MCP 工具调用流程的完整编排)
// ============================================================================
// 这是整个 MCP 协议的执行引擎, 实现了两阶段推理的完整流程。
//
// ┌─ 两阶段推理流程 ──────────────────────────────────────────────────┐
// │                                                                    │
// │  用户: "帮我查一下北京天气"                                         │
// │       │                                                            │
// │       ▼                                                            │
// │  【Phase 0: 策略选择】                                              │
// │    StrategyFactory::create(modelType) → 根据前端选择加载模型策略     │
// │    检查 isMCPModel 标志 → true=走MCP流程, false=走普通对话          │
// │       │                                                            │
// │       ▼                                                            │
// │  【Phase 1: 工具决策】                                              │
// │    ① buildPrompt(userInput) → 注入工具列表的提示词                  │
// │    ② messages.push_back(tempPrompt) → 临时注入                     │
// │    ③ buildRequest → curl → LLM API 第一次调用                      │
// │    ④ parseAIResponse → 判断 LLM 是否决定调用工具                    │
// │    ⑤ messages.pop_back() → 清理提示词 (不污染历史)                  │
// │       │                            │                               │
// │       ├── 不调用工具 ──────────────┘                               │
// │       │   持久化对话 → return LLM 回复                              │
// │       │                                                            │
// │       └── 调用工具                                                  │
// │             │                                                      │
// │             ▼                                                      │
// │  【工具执行】                                                       │
// │    ⑥ AIToolRegistry::invoke(toolName, args) → 执行工具 → 返回结果   │
// │             │                                                      │
// │             ▼                                                      │
// │  【Phase 2: 自然语言生成】                                          │
// │    ⑦ buildToolResultPrompt() → 注入工具结果的二次提示词             │
// │    ⑧ messages.push_back(secondPrompt) → 临时注入                   │
// │    ⑨ buildRequest → curl → LLM API 第二次调用                      │
// │    ⑩ messages.pop_back() → 清理提示词                              │
// │    ⑪ 持久化真实对话 → return LLM 自然语言回答                       │
// │                                                                    │
// └────────────────────────────────────────────────────────────────────┘
//
// 【关键设计决策】
//   为什么临时提示词要 push/pop 而不是直接拼接?
//     → 保持消息栈干净: 历史中只保存真实对话, 不保存系统提示词
//     → 防止 token 膨胀: 提示词如果留在历史中, 每次请求都会附带, 随时间累积
//     → LLM 上下文纯净: 后续请求不需要看到之前的 "工具调用协议文本"
//
//   为什么第一次 prompt 和第二次 prompt 是独立的两次 API 调用?
//     → LLM 不能自己执行工具, 只能 "表达意图"
//     → 中间需要 C++ 代码实际执行工具 (curl API / 系统调用)
//     → 这是 MCP 与纯对话的本质区别: 人(代码)在回路中
// ============================================================================
std::string AIHelper::chat(int userId,std::string userName, std::string sessionId, std::string userQuestion, std::string modelType) {

    // ── Step 0: 根据前端传入的 modelType 动态选择 AI 策略 ───────────────
    // modelType 可能的值: "1"=阿里百炼, "2"=豆包, "3"=阿里百炼RAG, "4"=阿里百炼MCP
    setStrategy(StrategyFactory::instance().create(modelType));

    // ═══════════════════════════════════════════════════════════════════════
    // 路径A: 非 MCP 模型 — 直接对话, 无工具调用
    // ═══════════════════════════════════════════════════════════════════════
    if (false == strategy->isMCPModel) {

        addMessage(userId, userName, true, userQuestion, sessionId);  // 持久化用户消息
        json payload = strategy->buildRequest(this->messages);        // 构建 API 请求

        //执行请求 — curl 同步阻塞 (muduo IO 线程在此等待)
        json response = executeCurl(payload);
        std::string answer = strategy->parseResponse(response);
        addMessage(userId, userName, false, answer, sessionId);      // 持久化 AI 回复
        return answer.empty() ? "[Error] 无法解析响应" : answer;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 路径B: MCP 模型 — 两阶段推理 + 工具调用
    // ═══════════════════════════════════════════════════════════════════════

    // ── Phase 1: 构建含工具列表的提示词, 让 LLM 决策是否调用工具 ──────────

    // Step 1.1: 加载配置并构建提示词
    // config.json → promptTemplate_ + tools_ → buildPrompt (填充占位符)
    AIConfig config;
    config.loadFromFile("../AIApps/ChatServer/resource/config.json");
    std::string tempUserQuestion = config.buildPrompt(userQuestion);
    std::cout << "tempUserQuestion is " << tempUserQuestion << std::endl;

    // Step 1.2: 临时注入提示词到消息栈
    // 【关键】这个提示词只是给 LLM 看的"元指令", 不是用户真实消息
    //          用完必须 pop, 否则会污染对话历史
    messages.push_back({ tempUserQuestion, 0 });

    // Step 1.3: 第一次 LLM API 调用 — 决策阶段
    // LLM 此时看到的是: "你是中间人...可用工具有...用户说了XX...需要调用工具吗?"
    json firstReq = strategy->buildRequest(this->messages);
    json firstResp = executeCurl(firstReq);
    std::string aiResult = strategy->parseResponse(firstResp);

    // Step 1.4: 立即清理提示词 — 不让系统指令残留
    messages.pop_back();

    std::cout << "aiResult is " << aiResult << std::endl;

    // Step 1.5: 解析 LLM 的决策结果
    // 三种可能:
    //   ① JSON{"tool":"get_weather","args":{...}} → isToolCall=true
    //   ② JSON{"reply":"..."} 或纯文本回复        → isToolCall=false
    //   ③ 非JSON/格式错误                          → isToolCall=false (安全回退)
    AIToolCall call = config.parseAIResponse(aiResult);

    // ── 情况1: LLM 决定不调用工具 → 当作普通对话处理 ─────────────────
    if (!call.isToolCall) {
        addMessage(userId, userName, true, userQuestion, sessionId);
        addMessage(userId, userName, false, aiResult, sessionId);

        std::cout << "No tools required" << std::endl;
        return aiResult;
    }

    // ── 情况2: LLM 决定调用工具 → 执行工具 → 二次推理 ─────────────────
    // Step 2.1: 通过 AIToolRegistry 执行 LLM 请求的工具
    json toolResult;
    AIToolRegistry registry;

    try {
        // 调用工具: 字符串 toolName → 哈希查找 → 调用 C++ 函数 → 返回 json
        toolResult = registry.invoke(call.toolName, call.args);
        std::cout << "Tool call success" << std::endl;
    }
    catch (const std::exception& e) {
        // 工具执行异常 (多数情况是工具未注册, 或 curl 失败)
        // 不会崩溃 — 将错误信息返回给前端
        std::string err = "[工具调用失败] " + std::string(e.what());
        addMessage(userId, userName, true, userQuestion, sessionId);
        addMessage(userId, userName, false, err, sessionId);

        std::cout << "Tool call failed" << std::endl << std::string(e.what());
        return err;
    }

    // Step 2.2: 构建二次推理提示词 (包含工具执行结果)
    // 告诉 LLM: "用户问了XX, 你决定调用XX工具, 工具返回了XX结果,
    //            请基于这些信息用自然语言回答用户"
    std::string secondPrompt = config.buildToolResultPrompt(
        userQuestion, call.toolName, call.args, toolResult);

    std::cout << "secondPrompt is " << secondPrompt << std::endl;

    // Step 2.3: 临时注入二次提示词 → 第二次 LLM 调用 → 清理
    messages.push_back({ secondPrompt, 0 });

    json secondReq = strategy->buildRequest(messages);
    json secondResp = executeCurl(secondReq);
    std::string finalAnswer = strategy->parseResponse(secondResp);

    // Step 2.4: 清理二次提示词 — 同样不能污染历史
    messages.pop_back();

    std::cout << "finalAnswer is " << finalAnswer << std::endl;

    // Step 2.5: 持久化真正的对话 (用户原始问题 + AI最终回答)
    // 注意: 持久化的是 userQuestion (原始输入) 和 finalAnswer (最终答案)
    //       中间的提示词和工具调用过程不保存
    addMessage(userId, userName, true, userQuestion, sessionId);
    addMessage(userId, userName, false, finalAnswer, sessionId);
    return finalAnswer;

}

// ============================================================================
// request — 发送原始 JSON 负载到 AI API (用于自定义请求)
// ============================================================================
json AIHelper::request(const json& payload) {
    return executeCurl(payload);
}

// ============================================================================
// GetMessages — 获取当前会话的完整对话历史
// ============================================================================
// 返回值: vector<pair<content, timestamp_ms>>
// 用于 ChatHistoryHandler 向前端返回历史消息
// 注意: 偶数索引为用户消息, 奇数索引为 AI 回复 (交替存储)
std::vector<std::pair<std::string, long long>> AIHelper::GetMessages() {
    return this->messages;
}


// ============================================================================
// executeCurl — 通过 libcurl 发送 HTTP POST 请求到 LLM API
// ============================================================================
// 所有 LLM API 调用的底层实现。
//
// 请求格式:
//   POST {strategy->getApiUrl()}
//   Header: Authorization: Bearer {strategy->getApiKey()}
//   Header: Content-Type: application/json
//   Body:   {strategy->buildRequest()} 的序列化 JSON
//
// 异常: curl 失败或 JSON 解析失败时抛出 runtime_error
//       调用方 AIHelper::chat() 不捕获此异常 → 传播到 ChatSendHandler
//       → HttpServer::handleRequest 捕获 → 返回 500
// ============================================================================
json AIHelper::executeCurl(const json& payload) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize curl");
    }

    std::cout<<"test "<< strategy->getApiUrl().c_str()<<' '<< strategy->getApiKey()<<std::endl;

    std::string readBuffer;
    struct curl_slist* headers = nullptr;

    // 构造 Authorization 头: "Bearer sk-xxxx"
    std::string authHeader = "Authorization: Bearer " + strategy->getApiKey();

    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string payloadStr = payload.dump();  // json → 字符串序列化


    curl_easy_setopt(curl, CURLOPT_URL, strategy->getApiUrl().c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payloadStr.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);         // ← 释放 curl_slist 链表
    curl_easy_cleanup(curl);             // ← 释放 curl 句柄

    if (res != CURLE_OK) {
        throw std::runtime_error("curl_easy_perform() failed: " + std::string(curl_easy_strerror(res)));
    }

    try {
        return json::parse(readBuffer);  // 将响应字符串解析为 JSON
    }
    catch (...) {
        throw std::runtime_error("Failed to parse JSON response: " + readBuffer);
    }
}

// ============================================================================
// WriteCallback — libcurl 响应数据写入回调
// ============================================================================
// 每次接收到数据块时被 libcurl 调用
// 将数据追加到 userp 指向的 std::string 缓冲区
size_t AIHelper::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::string* buffer = static_cast<std::string*>(userp);
    buffer->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

// ============================================================================
// escapeString — SQL 字符串转义 (防注入的基础层)
// ============================================================================
// 转义特殊字符: \ → \\, ' → \', " → \", \n → \\n, \r → \\r, \t → \\t
//
// 【安全说明】
//   这是一个基础的黑名单转义方案，不是 100% 安全。
//   生产环境下推荐使用参数化查询 (PreparedStatement) 从根本上防止 SQL 注入。
//   当前项目使用的是字符串拼接 (见 pushMessageToMysql)，
//   将 SQL 发送到 RabbitMQ 后由消费者执行，
//   escapeString 是第一道防线。
// ============================================================================
std::string AIHelper::escapeString(const std::string& input) {
    std::string output;
    output.reserve(input.size() * 2);   // 预分配内存: 最坏情况每个字符都转义
    for (char c : input) {
        switch (c) {
            case '\\': output += "\\\\"; break;  // 反斜杠必须最先处理
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


// ============================================================================
// pushMessageToMysql — 异步持久化消息到 MySQL (通过 RabbitMQ)
// ============================================================================
// 这是 AIHelper 与 MQManager 的对接点。
//
// 流程:
//   1. escapeString 转义输入 (第一道防线)
//   2. 拼接 SQL INSERT 语句
//   3. MQManager::publish("sql_queue", sql) → RabbitMQ → 消费者线程 → MySQL
//
// 【为什么用 RabbitMQ 而不是直接写 MySQL?】
//   问题: AI API 调用耗时 2-10 秒, 在此期间阻塞等待 MySQL 写入不必要
//   方案: 内存写入 + 消息队列异步入库
//   效果: 用户感知延迟只包含 AI API 时间, 数据库写入完全异步
//
// 【为什么用 RabbitMQ 而不是 std::thread 直接异步?】
//   1. 持久化: RabbitMQ 消息落盘, 进程崩溃不丢
//   2. 削峰: 高并发时消息在队列堆积, 消费者按自己节奏消费
//   3. 解耦: 消费者线程数量独立调整, 不影响请求处理线程
//   4. ACK 确认: 消费者处理成功才确认, 保证可靠投递
// ============================================================================
void AIHelper::pushMessageToMysql(int userId, const std::string& userName, bool is_user, const std::string& userInput,long long ms, std::string sessionId) {
    // 原始的直接执行 SQL 方式 (已废弃, 改为消息队列):
    // std::string sql = "INSERT INTO chat_message (id, username, is_user, content, ts) VALUES ("
    //     + std::to_string(userId) + ", "
    //     + "'" + userName + "', "
    //     + std::to_string(is_user ? 1 : 0) + ", "
    //     + "'" + userInput + "', "
    //     + std::to_string(ms) + ")";
    // mysqlUtil_.executeUpdate(sql);

    std::string safeUserName = escapeString(userName);
    std::string safeUserInput = escapeString(userInput);

    // 拼接 SQL INSERT 语句
    // 存储字段: id(用户ID), username, session_id, is_user(0/1), content, ts(毫秒时间戳)
    std::string sql = "INSERT INTO chat_message (id, username, session_id, is_user, content, ts) VALUES ("
        + std::to_string(userId) + ", "
        + "'" + safeUserName + "', "
        + sessionId + ", "
        + std::to_string(is_user ? 1 : 0) + ", "
        + "'" + safeUserInput + "', "
        + std::to_string(ms) + ")";

    //改成消息队列异步执行mysql操作，用于流量削峰与解耦逻辑
    //mysqlUtil_.executeUpdate(sql);

    // 投递到 RabbitMQ: "sql_queue" → RabbitMQThreadPool 的 2 个 worker 消费
    MQManager::instance().publish("sql_queue", sql);
}
