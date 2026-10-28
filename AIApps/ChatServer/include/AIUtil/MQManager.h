#pragma once

#include <amqpcpp.h>
#include <amqpcpp/linux_tcp/tcpparent.h>
#include <amqpcpp/linux_tcp/tcphandler.h>
#include <amqpcpp/linux_tcp/tcpconnection.h>
#include <amqpcpp/linux_tcp/tcpchannel.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/Channel.h>
#include <muduo/net/TimerId.h>
#include <functional>
#include <memory>
#include <string>
#include <queue>
#include <mutex>
#include <atomic>

// ============================================================
// MQManager — 基于 AMQP-CPP + muduo EventLoop 的 RabbitMQ 客户端
//
// 架构：
//   1 个 TCP 连接（AMQP::TcpConnection）
//    ├── Producer Channel — 发送消息到队列
//    └── Consumer Channel — 从队列接收消息
//
// 所有 I/O 由 muduo EventLoop epoll 驱动，无需额外线程。
//
// 之前：SimpleAmqpClient（5 个独立 TCP 连接池 + 2 个消费者线程）
// 之后：AMQP-CPP（1 个连接 + 2 个 Channel，事件驱动，零额外线程）
// ============================================================

class MQManager {
public:
    // ---- 对外类型 ----

    // 消费者消息回调
    //   msg         — 消息体（原始字节）
    //   deliveryTag — AMQP 投递标签，唯一标识这条消息
    //   ackFn       — 确认函数：ackFn(true)=ACK 确认删除, ackFn(false)=REJECT 重新入队
    //                 线程安全：内部自动切回 EventLoop，可在任意线程调用
    using MessageHandler = std::function<void(const std::string& msg,
                                              uint64_t deliveryTag,
                                              std::function<void(bool ack)>)>;

    // ---- 单例 ----
    static MQManager& instance() {
        static MQManager mgr;
        return mgr;
    }

    ~MQManager();

    // ---- 对外接口 ----

    // 启动：连接 RabbitMQ、声明队列、开始消费
    //   loop       — muduo EventLoop 指针（连接的所有 I/O 都注册在这个 loop 上）
    //   queueName  — 要声明和消费的队列名（如 "sql_queue"）
    //   onMessage  — 收到消息时的回调（在 EventLoop 线程中同步调用）
    // 必须在 EventLoop 线程中调用，应在 server.start() 之前调用
    void start(muduo::net::EventLoop* loop,
               const std::string& queueName,
               MessageHandler onMessage);

    // 发布消息到队列
    //   queue — 目标队列名
    //   msg   — 消息内容（纯文本或 SQL 语句）
    // 线程安全：可在任意线程调用，内部通过 queueInLoop 委托给 EventLoop 线程
    void publish(const std::string& queue, const std::string& msg);

    // 优雅关闭：断开连接、清理 Channel 和定时器
    void shutdown();

private:
    MQManager();

    MQManager(const MQManager&) = delete;
    MQManager& operator=(const MQManager&) = delete;

    // ============================================================
    // Handler — AMQP::TcpHandler 实现，muduo epoll 集成核心
    //
    // 负责：
    //   1. 接收 AMQP-CPP 的连接事件（onConnected / onLost / onError）
    //   2. 把 AMQP 的 socket fd 注册到 muduo EventLoop（monitor）
    //   3. 管理 Producer + Consumer 两个 AMQP Channel
    //   4. 断连时自动重连（指数退避）
    //   5. 断连期间暂存待发消息（pendingQueue_）
    // ============================================================
    class Handler : public AMQP::TcpHandler {
    public:
        // loop       — muduo EventLoop，所有 AMQP I/O 在此 loop 上处理
        // address    — RabbitMQ 连接地址（amqp://user:pass@host:port/vhost）
        // queueName  — 要消费的队列名
        // onMessage  — 业务层消息回调
        Handler(muduo::net::EventLoop* loop,
                const AMQP::Address& address,
                const std::string& queueName,
                MessageHandler onMessage);
        ~Handler();

        // 发布消息（必须在 EventLoop 线程调用，由 schedulePublish 委托过来）
        // 返回 true=已发送或已暂存, false=队列满丢弃
        bool publishOnLoop(const std::string& queue, const std::string& msg);

        // 发起 TCP 连接到 RabbitMQ
        void connect();

        // 断开连接并清理所有 Channel（不清理 pendingQueue_，消息不丢）
        void disconnect();

        // 跨线程发布入口：将 publish 任务委托给 EventLoop 线程执行
        void schedulePublish(std::string queue, std::string msg);

    private:
        // ---- AMQP::TcpHandler 回调接口 ----

        // TCP 连接建立且 AMQP 握手完成后调用
        void onConnected(AMQP::TcpConnection* connection) override;

        // TCP 连接断开后调用（仅在 onConnected 之后才会触发）
        void onLost(AMQP::TcpConnection* connection) override;

        // 连接发生错误时调用
        // 注意：如果 onConnected 从未触发，onLost 不会跟随调用，因此这里也触发重连
        void onError(AMQP::TcpConnection* connection, const char* message) override;

        // AMQP-CPP 请求监听/停止监听某个 socket fd
        // fd    — 要监听的 socket 文件描述符（-1 表示停止）
        // flags — AMQP::readable | AMQP::writable 的位掩码，0 表示停止监听
        void monitor(AMQP::TcpConnection* connection, int fd, int flags) override;

        // ---- 内部方法 ----

        // 创建 Producer Channel 并注册 onReady/onError 回调
        void setupProducer();

        // 创建 Consumer Channel → 声明持久化队列 → 设置 QoS=1 → 开始消费
        void startConsumer();

        // 消费回调入口：从 AMQP Message 中提取 body，构造 ackFn，调用业务 handler
        void onConsumerMessage(const AMQP::Message& msg, uint64_t deliveryTag, bool redelivered);

        // 调度重连（指数退避：5s → 10s → 20s → ... → 60s）
        // 通过 reconnectScheduled_ 防止重复调度
        void scheduleReconnect();

        // 排空暂存消息队列：将 pendingQueue_ 中的消息逐条 publish
        // 缓冲区满时停止（剩余消息等下次 writable 回调继续）
        void drainPending();

        // ---- 成员变量 ----

        muduo::net::EventLoop* loop_;           // muduo 事件循环（不拥有）
        AMQP::Address address_;                  // RabbitMQ 连接地址
        std::string queueName_;                  // 队列名
        MessageHandler messageHandler_;          // 业务层消息回调
        std::shared_ptr<bool> alive_ =           // 跨线程生命周期标志
            std::make_shared<bool>(true);        //   ~Handler 时置 false，queueInLoop/timer 回调检查

        std::unique_ptr<AMQP::TcpConnection> connection_;     // AMQP TCP 连接
        std::unique_ptr<AMQP::TcpChannel> producerChannel_;   // 生产者 Channel
        std::unique_ptr<AMQP::TcpChannel> consumerChannel_;   // 消费者 Channel
        std::unique_ptr<muduo::net::Channel> muduoChannel_;   // muduo epoll 封装（包装 AMQP socket fd）
        muduo::net::TimerId reconnectTimerId_;                // 重连定时器 ID

        int currentFd_ = -1;                    // 当前被监听的 socket fd（-1=未监听）
        std::string consumerTag_;               // AMQP 消费者标签（服务端分配）
        bool producerReady_ = false;            // Producer Channel 是否就绪
        bool consumerReady_ = false;            // Consumer Channel 是否就绪
        bool shuttingDown_ = false;             // 正在关闭中
        bool reconnectScheduled_ = false;       // 重连定时器是否已调度（防重复）
        int  reconnectDelaySec_ = 5;            // 当前重连等待秒数（指数退避：5→10→20→...→60）

        // 连接就绪前暂存的消息（如断连期间收到的 publish 请求）
        // key=队列名, value=消息体
        std::mutex pendingMutex_;               // 保护 pendingQueue_（仅在 EventLoop 线程访问，锁冗余但安全）
        std::queue<std::pair<std::string, std::string>> pendingQueue_;
    };

    std::unique_ptr<Handler> handler_;          // Handler 实例（单连接 + 双 Channel）
    muduo::net::EventLoop* loop_ = nullptr;     // 保存一份 loop 指针供外层使用
};
