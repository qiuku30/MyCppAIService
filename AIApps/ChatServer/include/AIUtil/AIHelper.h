#pragma once
#include <string>
#include <vector>
#include <utility>
#include <functional>
#include <mutex>
#include <curl/curl.h>
#include <iostream>
#include <sstream>

#include "../../../../HttpServer/include/utils/JsonUtil.h"
#include"../../../../HttpServer/include/utils/MysqlUtil.h"

#include"AIFactory.h"
#include"AIConfig.h"
#include"AIToolRegistry.h"


//这边封装curl去访问对阿里的模型
class AIHelper {
public:
    // 构造函数，初始化API Key
    AIHelper();

    // 设置默认模型
    //void setModel(const std::string& modelName);

    void setStrategy(std::shared_ptr<AIStrategy> strat);

    // 恢复一条消息
    void restoreMessage(const std::string& userInput, long long ms);
    void restoreMessage(const std::string& content, long long ms, bool isUser);

    struct HistoryMessage {
        std::string content;
        long long timestamp;
        bool isUser;
    };
    std::vector<HistoryMessage> GetHistorySnapshot();

    // 发送聊天消息，返回AI的响应内容
    // messages: [{"role":"system","content":"..."}, {"role":"user","content":"..."}]
    std::string chat(int userId, std::string userName, std::string sessionId, std::string userQuestion, std::string modelType);

    // 流式聊天：on_token 每收到一个 token 文本就回调，on_done 生成结束
    // 调用线程中执行（应由独立线程调用以避免阻塞 muduo IO）
    void chatStream(int userId, std::string userName, std::string sessionId,
                    std::string userQuestion, std::string modelType,
                    std::function<void(const std::string& token)> on_token,
                    std::function<void(const std::string& full_answer)> on_done,
                    std::function<void(const std::string& error)> on_error);

    // 可选：发送自定义请求体
    json request(const json& payload);

    std::vector<std::pair<std::string, long long>> GetMessages();

private:
    // 内部方法：执行curl请求，返回原始JSON（非流式）
    json executeCurl(const json& payload);
    // 内部方法：执行curl流式请求，逐 chunk 回调处理 SSE 数据
    void executeCurlStream(const json& payload,
                           std::function<void(const std::string& delta)> on_chunk,
                           std::function<void()> on_done,
                           std::function<void(const std::string&)> on_error);
    // curl 回调函数，把返回的数据写到 string buffer
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);

private:

    /*
    * 重构代码，将其使用策略模式&&工厂模式抽离出来
    std::string apiKey_;
    //默认用通义千问
    std::string model_ = "qwen-plus";
    //对应地址
    std::string apiUrl_ = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
    */
    std::shared_ptr<AIStrategy> strategy;

    // Preserve stored roles separately from the legacy content/timestamp API.
    // Models that infer roles from parity only accept complete alternating exchanges.
    std::vector<std::pair<std::string, long long>> messages;
    std::vector<bool> messageRoles_;
    void validateCanonicalHistory() const;

    // 串行化同一 session 的并发请求，防止消息历史交错
    std::mutex chat_mutex_;

    void commitExchange(int userId, const std::string& userName, const std::string& sessionId,
                        const std::string& question, const std::string& answer);

    //http::MysqlUtil mysqlUtil_;
};
