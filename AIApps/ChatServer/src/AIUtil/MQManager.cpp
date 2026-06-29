#include"../include/AIUtil/MQManager.h"
#include <cstdlib>

// ---- MQManager: 生产者连接池 ----

MQManager::MQManager(size_t poolSize)
    : poolSize_(poolSize), counter_(0) {
    for (size_t i = 0; i < poolSize_; ++i) {
        auto conn = std::make_shared<MQConn>();

        const char* mq_host = std::getenv("RABBITMQ_HOST");
        std::string host = mq_host ? mq_host : "localhost";
        const char* mq_user = std::getenv("RABBITMQ_USER");
        const char* mq_pass = std::getenv("RABBITMQ_PASS");

        conn->channel = AmqpClient::Channel::Create(host, 5672,
            mq_user ? mq_user : "guest",
            mq_pass ? mq_pass : "guest",
            "/");

        pool_.push_back(conn);
    }
}

void MQManager::publish(const std::string& queue, const std::string& msg) {
    // 原子轮询选 Channel，无锁分发
    size_t index = counter_.fetch_add(1) % poolSize_;

    auto& conn = pool_[index];
    std::lock_guard<std::mutex> lock(conn->mtx);

    auto message = AmqpClient::BasicMessage::Create(msg);
    conn->channel->BasicPublish("", queue, message);
}


// ---- RabbitMQThreadPool: 消费者线程池 ----

void RabbitMQThreadPool::start() {
    for (int i = 0; i < thread_num_; ++i) {
        workers_.emplace_back(&RabbitMQThreadPool::worker, this, i);
    }
}

void RabbitMQThreadPool::shutdown() {
    stop_ = true;
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

void RabbitMQThreadPool::worker(int id) {
    try {
        const char* mq_host = std::getenv("RABBITMQ_HOST");
        std::string host = mq_host ? mq_host : (rabbitmq_host_.empty() ? "localhost" : rabbitmq_host_);
        const char* mq_user = std::getenv("RABBITMQ_USER");
        const char* mq_pass = std::getenv("RABBITMQ_PASS");

        auto channel = AmqpClient::Channel::Create(host, 5672,
            mq_user ? mq_user : "guest",
            mq_pass ? mq_pass : "guest",
            "/");

        // durable=true, exclusive=false 支持多消费者共享
        channel->DeclareQueue(queue_name_, false, true, false, false);

        std::string consumer_tag = channel->BasicConsume(queue_name_, "", true, false, false);

        // QoS=1: 每次只预取一条，自然负载均衡
        channel->BasicQos(consumer_tag, 1);

        while (!stop_) {
            AmqpClient::Envelope::ptr_t env;
            bool ok = channel->BasicConsumeMessage(consumer_tag, env, 500);
            if (ok && env) {
                std::string msg = env->Message()->Body();
                handler_(msg);
                channel->BasicAck(env);
            }
        }

        channel->BasicCancel(consumer_tag);
    }
    catch (const std::exception& e) {
        std::cerr << "Thread " << id << " exception: " << e.what() << std::endl;
    }
}
