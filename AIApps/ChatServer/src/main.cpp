#include <string>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <chrono>
#include <ctime>
#include <fstream>
#include <functional>
#include <muduo/net/TcpServer.h>
#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>

#include"../include/ChatServer.h"
#include"../include/AIUtil/GemmaInferencePool.h"
#include"../include/AIUtil/MQManager.h"
#include"../include/AIUtil/ChatMessage.h"

const std::string QUEUE_NAME = "sql_queue";

// MQ 消费者回调：收到 SQL 后执行
// 新签名：AMQP-CPP 集成，包含 deliveryTag + ackFn（业务方自行决定 ACK/NACK）
void executeMysql(const std::string& body, uint64_t /*deliveryTag*/, MQManager::AckFn done) {
    static http::MysqlUtil database;
    try {
        auto exchange = ChatExchange::parse(body);
        exchange.persist(database);
        done(MQManager::Disposition::Ack);
    } catch (const std::invalid_argument&) {
        // Old SQL backlog and malformed messages are quarantined, never executed.
        done(MQManager::Disposition::Reject);
    } catch (const std::exception&) {
        LOG_ERROR << "Chat persistence failed; message retained for retry";
        done(MQManager::Disposition::Retry);
    }
}

// 带重试的依赖等待：轮询直到 MySQL 可用
static bool waitForDependencies(ChatServer& server, int maxRetries = 30) {
    for (int retry = 0; retry < maxRetries; ++retry) {
        try {
            server.initChatMessage();
            LOG_INFO << "Chat history loaded from MySQL successfully";
            return true;
        } catch (const std::exception& e) {
            LOG_WARN << "Waiting for MySQL history restore... (attempt " << (retry + 1)
                     << "/" << maxRetries << "): " << e.what();
            if (retry + 1 < maxRetries)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    LOG_ERROR << "Failed to restore MySQL chat history after " << maxRetries
              << " attempts; server startup cancelled";
    return false;
}


int main(int argc, char* argv[]) {
	LOG_INFO << "pid = " << getpid();
	std::string serverName = "ChatServer";
	int port = 80;
    //
    int opt;
    const char* str = "p:";
    while ((opt = getopt(argc, argv, str)) != -1)
    {
        switch (opt)
        {
        case 'p':
        {
            port = atoi(optarg);
            break;
        }
        default:
            break;
        }
    }
    muduo::Logger::setLogLevel(muduo::Logger::WARN);
    try {
        ChatServer server(port, serverName);
        server.setThreadNum(4);

        // Restore a complete history snapshot before any requests are served.
        if (!waitForDependencies(server)) return EXIT_FAILURE;

        // 初始化 Gemma 本地推理引擎（非致命：失败不影响其他模型）
        {
            const char* gemma_model = std::getenv("GEMMA_MODEL_PATH");
            const char* gemma_tokenizer = std::getenv("GEMMA_TOKENIZER_PATH");
            std::string model_path = gemma_model
                ? gemma_model
                : "/home/qiuku/gemma.cpp/build/2.0-2b-it-sfp.sbs";
            std::string tokenizer_path = gemma_tokenizer
                ? gemma_tokenizer
                : "/home/qiuku/gemma.cpp/build/tokenizer.spm";
            try {
                GemmaInferencePool::instance().initialize(model_path, tokenizer_path);
                LOG_INFO << "GemmaInferencePool started, model: " << model_path;
            } catch (const std::exception& e) {
                LOG_WARN << "GemmaInferencePool not available (non-fatal): " << e.what();
            }
        }

        // AMQP-CPP + muduo 集成：单连接双 Channel，所有 I/O 由 EventLoop epoll 驱动
        // 替代了旧的 SimpleAmqpClient 连接池 + 独立消费者线程池
        MQManager::instance().start(server.getLoop(), QUEUE_NAME, executeMysql);

        struct StopMQ { ~StopMQ() { MQManager::instance().shutdown(); } } stopMQ;
        server.start();
    } catch (const std::exception& error) {
        LOG_ERROR << "ChatServer initialization/startup failed: " << error.what();
        return EXIT_FAILURE;
    }
}
