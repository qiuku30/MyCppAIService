#include "../include/AIUtil/MQManager.h"
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <fstream>
#include <muduo/base/Logging.h>

// pendingQueue_ 容量上限：超过此值后新的 publish 请求会被丢弃并打 LOG_ERROR
// 防止 RabbitMQ 长时间不可用时内存无限增长
static constexpr size_t MAX_PENDING_SIZE = 10000;

// ============================================================
// MQManager::Handler 实现
// ============================================================

// ---- 构造 / 析构 ----

// 构造函数仅保存参数，不做任何 I/O 操作。
// 调用 connect() 才会真正发起 TCP 连接。
MQManager::Handler::Handler(muduo::net::EventLoop* loop,
                             const AMQP::Address& address,
                             const std::string& queueName,
                             MessageHandler onMessage)
    : loop_(loop)
    , address_(address)
    , queueName_(queueName)
    , messageHandler_(std::move(onMessage))
{
}

// 析构顺序很重要：
//   1. *alive_ = false → 阻止所有待处理的跨线程回调（queueInLoop + timer）
//   2. muduoChannel_.reset() → 从 epoll 移除监听
//   3. consumerChannel_.reset() → producerChannel_.reset() → 关闭 AMQP Channel
//   4. connection_.reset() → 关闭 TCP 连接
// 不调用 loop_->cancel() 是因为 MQManager 是 static 单例，析构晚于
// ChatServer（栈变量），此时 EventLoop 可能已销毁。定时器回调已有 alive 保护。
MQManager::Handler::~Handler() {
    *alive_ = false;
    shuttingDown_ = true;
    muduoChannel_.reset();
    consumerChannel_.reset();
    producerChannel_.reset();
    connection_.reset();
}

// 发起 TCP 连接到 RabbitMQ。
// 创建 TcpConnection 后，AMQP-CPP 自动开始非阻塞连接流程：
// DNS 解析 → TCP 握手 → AMQP 协议握手 → onConnected() 回调
void MQManager::Handler::connect() {
    connection_ = std::make_unique<AMQP::TcpConnection>(this, address_);
}

// 断开连接，清理所有 Channel 和 epoll 监听。
// pendingQueue_ 不清理——消息暂存，重连后继续发送。
// reset 顺序：muduoChannel（停 epoll）→ channels（停 AMQP）→ connection（断 TCP）
void MQManager::Handler::disconnect() {
    muduoChannel_.reset();
    consumerChannel_.reset();
    producerChannel_.reset();
    connection_.reset();
    producerReady_ = false;
    consumerReady_ = false;
}

// ---- AMQP::TcpHandler 回调 ----

// monitor — AMQP-CPP 请求监听 socket fd
//   fd    — 要监听的 fd（-1 无效）
//   flags — AMQP::readable（可读）/ AMQP::writable（可写）的位掩码
//           flags==0 表示停止监听
// 实现：用 muduo::net::Channel 包装 fd，注册到 EventLoop 的 epoll。
// fd 变化时（重连后新 socket）自动重建 Channel。
void MQManager::Handler::monitor(AMQP::TcpConnection* /*connection*/, int fd, int flags) {
    if (fd < 0) return;

    // 重连后 fd 变了 → 销毁旧 Channel，创建新的
    if (currentFd_ != fd) {
        muduoChannel_.reset();
        currentFd_ = fd;
    }

    if (flags == 0) {
        // AMQP-CPP 请求停止监听
        muduoChannel_.reset();
        currentFd_ = -1;
        return;
    }

    if (!muduoChannel_) {
        muduoChannel_ = std::make_unique<muduo::net::Channel>(loop_, fd);

        // fd 可读 → 数据从 RabbitMQ 到达 → 喂给 AMQP-CPP 解析
        muduoChannel_->setReadCallback([this](muduo::Timestamp) {
            if (connection_ && currentFd_ >= 0) {
                connection_->process(currentFd_, AMQP::readable);
            }
        });

        // fd 可写 → AMQP-CPP 内部 buffer 有数据要发送 → 发送完成后尝试排空 pendingQueue_
        muduoChannel_->setWriteCallback([this]() {
            if (connection_ && currentFd_ >= 0) {
                connection_->process(currentFd_, AMQP::writable);
                drainPending();
            }
        });

        // 对端关闭连接（记录日志，重连由 onLost/onError 处理）
        muduoChannel_->setCloseCallback([this]() {
            LOG_WARN << "AMQP connection closed by peer";
        });
    }

    // 按 AMQP-CPP 要求控制 epoll 监听开关
    if (flags & AMQP::readable) muduoChannel_->enableReading();
    else                       muduoChannel_->disableReading();

    if (flags & AMQP::writable) muduoChannel_->enableWriting();
    else                        muduoChannel_->disableWriting();
}

// onConnected — TCP 连接 + AMQP 握手完成后回调
// 在这里创建 Producer 和 Consumer 两个 Channel。
// 同时重置重连间隔（连接成功 → 退避归零）。
void MQManager::Handler::onConnected(AMQP::TcpConnection* /*connection*/) {
    reconnectDelaySec_ = 5;
    LOG_INFO << "MQManager connected to RabbitMQ";
    setupProducer();
    startConsumer();
}

// onLost — 连接断开后回调（仅在 onConnected 之后触发）
// 清理 + 调度重连。
void MQManager::Handler::onLost(AMQP::TcpConnection* /*connection*/) {
    LOG_WARN << "MQManager lost connection to RabbitMQ";
    disconnect();
    if (!shuttingDown_) {
        scheduleReconnect();
    }
}

// onError — 连接错误回调
// 注意：如果从未连上过（如 RabbitMQ 未启动），onLost 不会被跟随调用，
// 因此这里也需显式触发重连。reconnectScheduled_ 防止 onLost 重复调度。
void MQManager::Handler::onError(AMQP::TcpConnection* /*connection*/, const char* message) {
    LOG_ERROR << "MQManager connection error: " << message;
    disconnect();
    if (!shuttingDown_) {
        scheduleReconnect();
    }
}

// ---- Channel 管理 ----

// 创建 Producer Channel。
// 就绪后设置 producerReady_=true 并排空 pendingQueue_ 中的暂存消息。
void MQManager::Handler::setupProducer() {
    producerChannel_ = std::make_unique<AMQP::TcpChannel>(connection_.get());

    producerChannel_->onReady([this]() {
        producerReady_ = true;
        LOG_INFO << "MQManager producer channel ready";
        drainPending();
    });

    producerChannel_->onError([this](const char* msg) {
        producerReady_ = false;
        LOG_ERROR << "MQManager producer channel error: " << msg;
    });
}

// 创建 Consumer Channel。
// 流程：声明持久化队列 → 设置 QoS=1（每次预取一条，公平分发）→ 开始消费。
// 收到消息时通过 onReceived → onConsumerMessage → messageHandler_ 链回调业务层。
void MQManager::Handler::startConsumer() {
    consumerChannel_ = std::make_unique<AMQP::TcpChannel>(connection_.get());

    consumerChannel_->onReady([this]() {
        // durable=true: 队列在 RabbitMQ 重启后不丢失
        consumerChannel_->declareQueue(queueName_, AMQP::durable)
            .onSuccess([this](const std::string& /*name*/,
                              uint32_t /*msgCount*/, uint32_t /*consumerCount*/) {
                // QoS=1: 每次只预取一条消息，多个消费者可公平分发
                consumerChannel_->setQos(1);
                // 开始消费（consumerTag 由服务端自动分配）
                consumerChannel_->consume(queueName_)
                    .onReceived([this](const AMQP::Message& msg,
                                       uint64_t deliveryTag, bool /*redelivered*/) {
                        onConsumerMessage(msg, deliveryTag, false);
                    })
                    .onSuccess([this](const std::string& tag) {
                        consumerTag_ = tag;
                        consumerReady_ = true;
                        LOG_INFO << "MQManager consumer ready, tag=" << tag;
                    })
                    .onError([this](const char* error) {
                        consumerReady_ = false;
                        LOG_ERROR << "MQManager consume failed: " << error;
                    });
            })
            .onError([this](const char* error) {
                LOG_ERROR << "MQManager declareQueue failed: " << error;
            });
    });

    consumerChannel_->onError([this](const char* msg) {
        consumerReady_ = false;
        LOG_ERROR << "MQManager consumer channel error: " << msg;
    });
}

// ---- 消息回调 ----

// 从 AMQP Message 中提取 body，构造线程安全的 ackFn，回调业务 handler。
//   msg         — AMQP 消息对象
//   deliveryTag — 投递标签（用于 ACK/NACK）
//   redelivered — 是否为重新投递（当前未使用）
//
// ackFn 内置 queueInLoop，业务 handler 可在任意线程安全调用。
//
// 注意：messageHandler_ 在 EventLoop 线程中同步调用。
// 如果业务 handler 包含阻塞 I/O（如数据库写入），应将其投递到工作线程执行，
// 避免阻塞 EventLoop 导致心跳超时。
void MQManager::Handler::onConsumerMessage(const AMQP::Message& msg,
                                            uint64_t deliveryTag, bool /*redelivered*/) {
    std::string body(msg.body(), msg.bodySize());

    // 线程安全的 ack 函数：通过 queueInLoop 将 AMQP 操作切回 EventLoop 线程
    //   ack=true  → ACK 确认（消息从队列删除）
    //   ack=false → REJECT + requeue（消息重新入队，交给其他消费者）
    auto ackFn = [this, alive = alive_, deliveryTag](bool ack) {
        loop_->queueInLoop([this, alive, deliveryTag, ack]() {
            if (!*alive) return;
            if (!consumerChannel_) return;
            if (ack) {
                consumerChannel_->ack(deliveryTag);
            } else {
                consumerChannel_->reject(deliveryTag, AMQP::requeue);
            }
        });
    };

    try {
        if (messageHandler_) {
            messageHandler_(body, deliveryTag, std::move(ackFn));
        }
    } catch (const std::exception& e) {
        // 兜底：业务 handler 抛出未捕获异常 → 落盘到 failed_sql.log 并 ACK
        LOG_ERROR << "MQManager message handler threw: " << e.what()
                  << " | msg=" << body.substr(0, 200);
        {
            std::ofstream failedFile("failed_sql.log", std::ios::app);
            if (failedFile.is_open()) {
                auto now = std::chrono::system_clock::now();
                auto t = std::chrono::system_clock::to_time_t(now);
                char timeBuf[32];
                std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
                failedFile << "[" << timeBuf << "] error=" << e.what()
                           << " | " << body << std::endl;
            }
        }
        consumerChannel_->ack(deliveryTag);
    }
}

// ---- 重连 ----

// 指数退避重连：首次 5s，之后每次翻倍（5→10→20→40→60），上限 60s。
// reconnectScheduled_ 防止重复调度（应对 onError + onLost 连续触发）。
// 连接成功后 onConnected 中重置间隔为 5s。
void MQManager::Handler::scheduleReconnect() {
    if (shuttingDown_ || reconnectScheduled_) return;

    LOG_INFO << "MQManager scheduling reconnect in " << reconnectDelaySec_ << " seconds";
    reconnectScheduled_ = true;
    reconnectTimerId_ = loop_->runAfter(reconnectDelaySec_, [this, alive = alive_]() {
        if (!*alive) return;
        reconnectScheduled_ = false;
        if (!shuttingDown_) {
            LOG_INFO << "MQManager attempting reconnect...";
            reconnectDelaySec_ = std::min(reconnectDelaySec_ * 2, 60);
            connect();
        }
    });
}

// ---- 消息暂存与排空 ----

// 排空 pendingQueue_ 中的暂存消息。
// 在 Producer Channel 就绪时（onReady）调用，也在 socket 可写时（write callback）调用。
// 缓冲区满时停止（剩余消息留在队列等下次 writable 回调继续）。
void MQManager::Handler::drainPending() {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    size_t count = 0;
    while (!pendingQueue_.empty()) {
        auto& [queue, msg] = pendingQueue_.front();
        if (!producerChannel_->publish("", queue, msg)) {
            // AMQP-CPP 内部发送缓冲区满 → 停止，下次 writable 时继续
            LOG_WARN << "MQManager drainPending buffer full, "
                     << pendingQueue_.size() << " messages remaining";
            break;
        }
        pendingQueue_.pop();
        ++count;
    }
    if (count > 0) {
        LOG_INFO << "MQManager drained " << count << " pending messages";
    }
}

// ---- 发布 ----

// 线程安全的 publish 入口。
// 在任意线程调用 → 通过 queueInLoop 委托到 EventLoop 线程 → publishOnLoop 执行。
// 捕获 alive_ 确保 Handler 析构后回调安全跳过。
void MQManager::Handler::schedulePublish(std::string queue, std::string msg) {
    auto alive = alive_;
    loop_->queueInLoop([this, alive, q = std::move(queue), m = std::move(msg)]() {
        if (!*alive) return;
        publishOnLoop(q, m);
    });
}

// 在 EventLoop 线程中实际执行 publish。
//   queue — 目标队列名（通常是 "" 即默认队列）
//   msg   — 消息体
// 返回 true=已发送或已暂存，false=队列满丢弃。
//
// 三种路径：
//   1. Producer 就绪 → 直接 publish
//   2. Producer 未就绪或发送失败 → 暂存到 pendingQueue_（有容量上限）
//   3. pendingQueue_ 满 → 丢弃并打 LOG_ERROR
bool MQManager::Handler::publishOnLoop(const std::string& queue, const std::string& msg) {
    if (!producerReady_ || !producerChannel_) {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        if (pendingQueue_.size() >= MAX_PENDING_SIZE) {
            LOG_ERROR << "MQManager pending queue full (" << MAX_PENDING_SIZE
                      << "), dropping message: " << msg.substr(0, 100);
            return false;
        }
        pendingQueue_.emplace(queue, msg);
        return true;
    }

    bool ok = producerChannel_->publish("", queue, msg);
    if (!ok) {
        LOG_WARN << "MQManager publish buffer full, queuing";
        std::lock_guard<std::mutex> lock(pendingMutex_);
        if (pendingQueue_.size() >= MAX_PENDING_SIZE) {
            LOG_ERROR << "MQManager pending queue full, dropping: " << msg.substr(0, 100);
            return false;
        }
        pendingQueue_.emplace(queue, msg);
        return false;
    }
    return true;
}

// ============================================================
// MQManager 外层 — 单例 + 对外接口
// ============================================================

MQManager::MQManager() = default;

MQManager::~MQManager() {
    shutdown();
}

// 启动 MQManager。
// 从环境变量 RABBITMQ_HOST / RABBITMQ_USER / RABBITMQ_PASS 读取连接参数，
// 创建 Handler 并发起连接。连接过程和后续 I/O 都在传入的 EventLoop 上完成。
void MQManager::start(muduo::net::EventLoop* loop,
                       const std::string& queueName,
                       MessageHandler onMessage) {
    loop_ = loop;

    // 从环境变量读取 RabbitMQ 连接参数，未设则用默认值
    const char* mq_host = std::getenv("RABBITMQ_HOST");
    std::string host = mq_host ? mq_host : "localhost";
    const char* mq_user = std::getenv("RABBITMQ_USER");
    const char* mq_pass = std::getenv("RABBITMQ_PASS");

    std::string connStr = "amqp://"
        + std::string(mq_user ? mq_user : "guest") + ":"
        + std::string(mq_pass ? mq_pass : "guest") + "@"
        + host + ":5672/";

    AMQP::Address address(connStr);
    handler_ = std::make_unique<Handler>(loop, address, queueName, std::move(onMessage));
    handler_->connect();

    LOG_INFO << "MQManager started, queue=" << queueName
             << " host=" << host;
}

// 关闭 MQManager：销毁 Handler → 断开连接 → 清理所有资源
void MQManager::shutdown() {
    if (handler_) {
        handler_.reset();
    }
}

// 发布消息到 RabbitMQ 队列。
// 线程安全：内部通过 schedulePublish → queueInLoop 委托到 EventLoop 线程。
void MQManager::publish(const std::string& queue, const std::string& msg) {
    if (!handler_) {
        LOG_ERROR << "MQManager::publish called before start()";
        return;
    }
    handler_->schedulePublish(queue, msg);
}
