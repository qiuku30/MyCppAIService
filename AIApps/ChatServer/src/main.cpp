#include <string>
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

const std::string QUEUE_NAME = "sql_queue";

// MQ 消费者回调：收到 SQL 后执行
// 新签名：AMQP-CPP 集成，包含 deliveryTag + ackFn（业务方自行决定 ACK/NACK）
void executeMysql(const std::string& sql,
                  uint64_t /*deliveryTag*/,
                  std::function<void(bool)> ackFn) {
    static http::MysqlUtil mysqlUtil_;
    try {
        mysqlUtil_.executeUpdate(sql);
        ackFn(true);  // 成功 → ACK
    } catch (const std::exception& e) {
        // SQL 执行失败 → 落盘到 failed_sql.log，然后 ACK（防止无限重试）
        LOG_ERROR << "executeMysql failed: " << e.what()
                  << " | sql=" << sql.substr(0, 200);
        {
            std::ofstream failedFile("failed_sql.log", std::ios::app);
            if (failedFile.is_open()) {
                auto now = std::chrono::system_clock::now();
                auto t = std::chrono::system_clock::to_time_t(now);
                char timeBuf[32];
                std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
                failedFile << "[" << timeBuf << "] error=" << e.what()
                           << " | " << sql << std::endl;
            }
        }
        ackFn(true);  // 落盘后 ACK，不重试
    }
}

// 带重试的依赖等待：轮询直到 MySQL 可用
static void waitForDependencies(ChatServer& server, int maxRetries = 30) {
    for (int retry = 0; retry < maxRetries; ++retry) {
        try {
            server.initChatMessage();
            LOG_INFO << "Chat history loaded from MySQL successfully";
            return;
        } catch (const std::exception& e) {
            LOG_WARN << "Waiting for MySQL... (attempt " << (retry + 1)
                     << "/" << maxRetries << "): " << e.what();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    LOG_ERROR << "Failed to connect to MySQL after " << maxRetries
              << " retries, starting with empty history";
    // 不终止进程：服务仍可启动，只是历史记录为空
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
    ChatServer server(port, serverName);
    server.setThreadNum(4);

    // 带重试的 MySQL 依赖等待（替代固定 sleep(2)）
    // waitForDependencies(server);  // TODO: readDataFromMySQL PreparedStatement 问题待修复

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

    server.start();
}
