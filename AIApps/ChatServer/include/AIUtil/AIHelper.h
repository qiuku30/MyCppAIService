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

    // 添加一条消息
    void addMessage(int userId, const std::string& userName, bool is_user, const std::string& userInput, std::string sessionId);
    // 恢复一条消息
    void restoreMessage(const std::string& userInput, long long ms);

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
    std::string escapeString(const std::string& input);
    //加入到mysql的接口（提供加入到线程池的接口，线程池做异步mysql更新操作）
    //todo: 
    void pushMessageToMysql(int userId, const std::string& userName, bool is_user, const std::string& userInput, long long ms,std::string sessionId);

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

    //一个用户针对一个AIHelper，messages存放用户的历史对话
    //偶数下标代表用户的信息，奇数下标是ai返回的内容
    //后者代表时间戳
    std::vector<std::pair<std::string, long long>> messages;

    // 串行化同一 session 的并发请求，防止消息历史交错
    std::mutex chat_mutex_;

    // RAII 守卫：构造时 push 消息，析构时自动 pop，异常安全
    struct ScopedMessage {
        std::vector<std::pair<std::string, long long>>& msgs;
        ScopedMessage(decltype(msgs)& m, const std::string& content)
            : msgs(m) { msgs.push_back({content, 0}); }
        ~ScopedMessage() { msgs.pop_back(); }
        ScopedMessage(const ScopedMessage&) = delete;
        ScopedMessage& operator=(const ScopedMessage&) = delete;
    };

    //http::MysqlUtil mysqlUtil_;
};
