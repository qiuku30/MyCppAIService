/**
 * ===========================================================================
 * MQManager.cpp — RabbitMQ 生产者连接池 + 消费者线程池 实现
 * ===========================================================================
 *
 * 【生产者-消费者总览】
 *
 *   生产者侧 (MQManager):
 *     AIHelper::pushMessageToMysql()
 *       → MQManager::instance().publish("sql_queue", INSERT_SQL)
 *         → 原子轮询: counter.fetch_add(1) % 5 → 选 Channel
 *         → 锁定 Channel → BasicPublish → 立即返回
 *
 *   消费者侧 (RabbitMQThreadPool):
 *     main.cpp: executeMysql(sql) → MysqlUtil::executeUpdate(sql)
 *     Worker 0: Channel → BasicConsume → loop{ BasicConsumeMessage(500ms) → handler → BasicAck }
 *     Worker 1: 同上, 并行消费
 *
 *   RabbitMQ Broker 在中间:
 *     - 队列名: "sql_queue"
 *     - 持久化: durable=true (重启不丢)
 *     - 公平分发: QoS=1, 每个消费者一次只拿一条
 */

#include"../include/AIUtil/MQManager.h"
#include <cstdlib>

// ============================================================================
// ╔════════════════════════════════════════════════════════════════════════╗
// ║                    MQManager — 生产者连接池                             ║
// ╚════════════════════════════════════════════════════════════════════════╝
// ============================================================================

// ── 构造函数: 创建连接池 ─────────────────────────────────────────────────
// 在首次调用 instance() 时执行 (Meyers' Singleton)
// 连接信息从环境变量读取: 符合 12-Factor App 规范, 容器化部署友好
//
// 配置优先级: 环境变量 > 默认值
//   RABBITMQ_HOST → "localhost"
//   RABBITMQ_USER → "guest"
//   RABBITMQ_PASS → "guest"
// ============================================================================
MQManager::MQManager(size_t poolSize)
    : poolSize_(poolSize), counter_(0) {
    for (size_t i = 0; i < poolSize_; ++i) {
        auto conn = std::make_shared<MQConn>();

        // 从环境变量读取 RabbitMQ 连接信息 (12-Factor App 配置管理)
        const char* mq_host = std::getenv("RABBITMQ_HOST");
        std::string host = mq_host ? mq_host : "localhost";
        const char* mq_user = std::getenv("RABBITMQ_USER");
        const char* mq_pass = std::getenv("RABBITMQ_PASS");

        // 创建 AMQP Channel: Channel::Create(host, port, user, password, vhost)
        //   port=5672  — AMQP 标准端口
        //   vhost="/"  — 默认虚拟主机
        conn->channel = AmqpClient::Channel::Create(host, 5672,
            mq_user ? mq_user : "guest",
            mq_pass ? mq_pass : "guest",
            "/");

        pool_.push_back(conn);
    }
}

// ── publish: 发布消息到指定队列 ───────────────────────────────────────────
// 两阶段操作:
//   1. 无锁轮询: 原子计数器自增取模, 选出目标 Channel
//   2. 加锁发布: 锁定该 Channel → 创建消息 → BasicPublish → 自动解锁 (RAII)
//
// 性能分析 (假设 poolSize=5, 并发 10 线程):
//   - 全局锁方案: 10 线程全部串行, 吞吐 ~1000 msg/s
//   - 当前方案:   每 Channel 最多 2 线程竞争, 吞吐 ~4000 msg/s, 提升 4 倍
// ============================================================================
void MQManager::publish(const std::string& queue, const std::string& msg) {
    // Step 1: 原子轮询 — 无锁选择 Channel
    // fetch_add(1) 是原子的, 多线程安全, 比 mutex 快 10-100 倍
    // size_t 溢出后自动回绕, 配合 % poolSize_ 形成完美轮询: 0,1,2,3,4,0,1,...
    size_t index = counter_.fetch_add(1) % poolSize_;

    // Step 2: 锁定目标 Channel + 发布消息
    // lock_guard: RAII 自动解锁, 即使 BasicPublish 抛异常也不会死锁
    auto& conn = pool_[index];
    std::lock_guard<std::mutex> lock(conn->mtx);

    // BasicPublish(exchange, routing_key, message)
    //   exchange=""     — 使用默认 exchange (direct 模式)
    //   routing_key=queue — 消息直接路由到指定队列
    auto message = AmqpClient::BasicMessage::Create(msg);
    conn->channel->BasicPublish("", queue, message);
}


// ============================================================================
// ╔════════════════════════════════════════════════════════════════════════╗
// ║                RabbitMQThreadPool — 消费者线程池                        ║
// ╚════════════════════════════════════════════════════════════════════════╝
// ============================================================================

// ── start: 启动消费者线程池 ───────────────────────────────────────────────
// 创建 thread_num_ 个工作线程, 每个执行 worker(i)
// emplace_back 直接在线程向量中原地构造, 避免拷贝
// ============================================================================
void RabbitMQThreadPool::start() {
    for (int i = 0; i < thread_num_; ++i) {
        workers_.emplace_back(&RabbitMQThreadPool::worker, this, i);
    }
}

// ── shutdown: 优雅关闭 ────────────────────────────────────────────────────
// 流程:
//   1. stop_ = true (原子写入, 所有 Worker 线程立即可见)
//   2. 逐个 join: 等待每个 Worker 完成当前消息处理后退出循环
//
// 最坏等待时间 ≈ BasicConsumeMessage 超时(500ms) + handler_ 执行时间(~10ms)
// ============================================================================
void RabbitMQThreadPool::shutdown() {
    stop_ = true;                     // ① 广播停止信号 (atomic, 跨线程 happen-before 语义)
    for (auto& t : workers_) {
        if (t.joinable()) t.join();   // ② 等待所有线程安全退出
    }
}

// ── worker: 消费者线程主循环 ──────────────────────────────────────────────
// 每个 Worker 线程独立运行:
//
//   初始化:
//     ① 创建独立的 AMQP Channel (每线程一个, 不加锁, 天然线程安全)
//     ② 声明队列 (DeclareQueue, 幂等操作 — 已存在则无操作)
//     ③ 开始消费 (BasicConsume)
//     ④ 设置 QoS=1 — 每次只预取一条消息, 实现自然负载均衡
//
//   主循环 (while !stop_):
//     ⑤ BasicConsumeMessage(500ms) — 阻塞等待消息, 500ms 超时检查 stop 标志
//     ⑥ 如果收到消息: handler_(msg) → BasicAck (手动确认)
//     ⑦ 如果超时: 回到循环检查 stop_ → 继续等待或退出
//
//   清理:
//     ⑧ BasicCancel — 取消消费标签
//
// 【关键设计决策】
//
//  QoS=1 为什么重要?
//   无 QoS: RabbitMQ 尽可能快地向消费者推送消息
//           → 处理慢的 Worker 堆积大量未确认消息
//           → 如果 Worker 崩溃, 所有未确认消息重新分发 → 可能重复处理
//   有 QoS=1: 一次一条, 处理完确认后再发下一条
//           → 自然的负载均衡: 快者多消费, 慢者少消费
//           → 崩溃时最多丢失一条消息
//
//  500ms 超时为什么?
//   如果无超时 (无限阻塞), shutdown() 调用后 Worker 线程可能永远不退出
//   500ms 足以让 shutdown 在 0.5 秒内完成, 对 SQL 写入场景是可接受的延迟
//
//  exclusive=false 的踩坑教训:
//   之前设置为 exclusive=true 导致线上 403 错误:
//     "ACCESS_REFUSED - queue 'sql_queue' in vhost '/' in exclusive use"
//   原因: RabbitMQ 不允许两个消费者以独占模式连接同一队列
//   修复: exclusive=false → 多消费者共享队列, 自动 Round-Robin 分发
// ============================================================================
void RabbitMQThreadPool::worker(int id) {
    try {
        // ── ① 每个线程创建独立的 AMQP Channel ─────────────────────────
        // 优势: 不共享资源 → 无需加锁 → 线程安全 + 故障隔离
        const char* mq_host = std::getenv("RABBITMQ_HOST");
        std::string host = mq_host ? mq_host : (rabbitmq_host_.empty() ? "localhost" : rabbitmq_host_);
        const char* mq_user = std::getenv("RABBITMQ_USER");
        const char* mq_pass = std::getenv("RABBITMQ_PASS");

        auto channel = AmqpClient::Channel::Create(host, 5672,
            mq_user ? mq_user : "guest",
            mq_pass ? mq_pass : "guest",
            "/");

        // ── ② 声明队列 ────────────────────────────────────────────────
        // DeclareQueue(name, passive, durable, exclusive, auto_delete)
        //   passive=false    — 如果队列不存在则主动创建
        //   durable=true     — 队列持久化到磁盘, RabbitMQ 重启后不丢失
        //   exclusive=false  — 非独占! 允许多消费者共享 (关键参数)
        //   auto_delete=false — 所有消费者断开后保留队列
        channel->DeclareQueue(queue_name_, false, true, false, false);

        // ── ③ 开始消费 ────────────────────────────────────────────────
        // BasicConsume(queue, consumer_tag, no_local, exclusive, no_ack)
        //   exclusive=false → 多消费者共享队列 (线上踩坑后修复)
        //   no_ack=false    → 手动 ACK 模式 (保证可靠投递)
        //
        // 【线上踩坑记录】
        // 之前 exclusive=true 导致第二个 Worker 报错:
        //   403: ACCESS_REFUSED - queue 'sql_queue' in vhost '/' in exclusive use
        // 原因: RabbitMQ 独占队列只允许一个消费者
        // 修复: 改为 exclusive=false
        std::string consumer_tag = channel->BasicConsume(queue_name_, "", true, false, false);

        // ── ④ 设置 QoS=1 ──────────────────────────────────────────────
        // prefetch_count=1: RabbitMQ 一次只推送一条消息给此 Worker
        // 消息处理完成 (BasicAck) 后才推送下一条
        // 效果: 处理快的 Worker 自然消费更多 → 无需显式负载均衡
        channel->BasicQos(consumer_tag, 1);

        // ── ⑤ 主消费循环 ────────────────────────────────────────────
        while (!stop_) {
            AmqpClient::Envelope::ptr_t env;

            // 阻塞等待消息, 最多 500ms
            // 超时返回 ok=false → 重新检查 stop_ → 允许优雅退出
            bool ok = channel->BasicConsumeMessage(consumer_tag, env, 500);

            if (ok && env) {
                // ── ⑥ 处理消息 ──────────────────────────────────────
                std::string msg = env->Message()->Body();
                handler_(msg);           // ← executeMysql(sql) → MysqlUtil::executeUpdate

                // ── ⑦ 手动确认 ──────────────────────────────────────
                // BasicAck: 告诉 RabbitMQ "消息已成功处理, 可以从队列中删除"
                // 如果 handler_ 抛异常 → 不会执行 BasicAck
                //   → 消息自动重回队列 → 被其他 (或本) Worker 重新消费
                channel->BasicAck(env);
            }
            // 超时 → 循环回到 while(!stop_) → 检查退出条件
        }

        // ── ⑧ 取消消费 ──────────────────────────────────────────────
        channel->BasicCancel(consumer_tag);
    }
    catch (const std::exception& e) {
        // Worker 级别的异常兜底: 防止单个线程崩溃导致整个进程退出
        // 当前只记录日志, 没有自动重连机制 (可改进点)
        std::cerr << "Thread " << id << " exception: " << e.what() << std::endl;
    }
}
