/**
 * ===========================================================================
 * MQManager.h — RabbitMQ 消息队列管理 (生产者连接池 + 消费者线程池)
 * ===========================================================================
 *
 * 【本模块在系统中的作用】
 *   AIHelper 通过 MQManager 将 SQL INSERT 语句异步投递到 RabbitMQ,
 *   由 RabbitMQThreadPool 的消费者线程异步执行, 实现数据库写入与
 *   AI API 调用的完全解耦。
 *
 *   生产者-消费者架构:
 *     AIHelper (生产者) → MQManager (连接池) → RabbitMQ Broker
 *                                            ↓
 *     main.cpp (消费者) ← RabbitMQThreadPool ← "sql_queue"
 *                                            ↓
 *                                         MySQL
 *
 * 【为什么需要消息队列?】
 *   场景: AI API 调用耗时 2-10 秒, 期间需要持久化用户消息
 *   问题: 如果同步写 MySQL, 用户感受到的延迟 = AI耗时 + MySQL耗时
 *   方案: 内存写入瞬时完成 + RabbitMQ 异步入库
 *   效果: 用户感知延迟降低, 数据库写入削峰填谷
 *
 * 【两个核心类】
 *   MQManager          — 单例生产者, 5 通道连接池, 轮询分发
 *   RabbitMQThreadPool — 多线程消费者, QoS=1 公平分发, 手动 ACK
 */

#pragma once

#include <SimpleAmqpClient/SimpleAmqpClient.h>
#include <vector>
#include <mutex>
#include <memory>
#include <atomic>
#include <thread>
#include <iostream>
#include <chrono>
#include <functional>


// ============================================================================
// MQManager — 单例 RabbitMQ 生产者 (发布端)
// ============================================================================
// 设计要点:
//   1. Meyers' Singleton: C++11 保证的线程安全单例
//   2. 连接池 (默认 5 个 Channel): 避免单 Channel 的串行化瓶颈
//   3. 原子计数器轮询: 无锁选择 Channel, 每条消息走不同连接
//   4. 每 Channel 独立互斥锁: 5 个生产线程可并行发布, 互不阻塞
//   5. 环境变量配置: RABBITMQ_HOST/USER/PASS, 容器化部署友好
//
// 使用方式 (全局唯一单例):
//   MQManager::instance().publish("sql_queue", sql_statement);
// ============================================================================
class MQManager {
public:
    // ── 单例入口 (Meyers' Singleton, C++11 线程安全) ──────────────────
    static MQManager& instance() {
        static MQManager mgr; // 首次调用时构造, 进程结束时析构
        return mgr;
    }

    // ── 发布消息 ──────────────────────────────────────────────────────
    // queue — 目标队列名 ("sql_queue")
    // msg   — 消息体 (SQL INSERT 语句)
    // 内部: 原子轮询选 Channel → 加锁 → BasicPublish
    void publish(const std::string& queue, const std::string& msg);

private:
    // ── MQConn: 单个 RabbitMQ Channel + 独立互斥锁 ──────────────────
    // 为什么每个 Channel 一把锁而不是全局锁?
    //   全局锁 → 任何线程发布消息都阻塞其他所有线程 → 退化为串行
    //   每Channel锁 → 只有使用同一 Channel 的线程才互斥 → 并行度 = 池大小
    struct MQConn {
        AmqpClient::Channel::ptr_t channel; // RabbitMQ Channel (虚拟连接)
        std::mutex mtx;                     // 保护此 Channel 的互斥锁
    };

    // ── 构造函数 (私有 — 单例模式) ───────────────────────────────────
    // poolSize=5: 5 个 Channel 并发, 足够支撑每秒数千条消息
    MQManager(size_t poolSize = 5);

    // 禁止拷贝和赋值 (单例模式标准做法)
    MQManager(const MQManager&) = delete;
    MQManager& operator=(const MQManager&) = delete;

    // ── 成员变量 ─────────────────────────────────────────────────────
    std::vector<std::shared_ptr<MQConn>> pool_;  // Channel 连接池
    size_t poolSize_;                             // 池大小 (5)
    std::atomic<size_t> counter_;                 // 轮询计数器 (无锁原子变量)
};


// ============================================================================
// RabbitMQThreadPool — 多线程 RabbitMQ 消费者 (订阅端)
// ============================================================================
// 设计要点:
//   1. 每线程独立 Channel: 无需加锁, 天然线程安全
//   2. QoS=1 预取: 每个 Worker 一次只取一条消息, 实现自然的负载均衡
//   3. 手动 ACK: 消息处理成功后才确认, 处理失败则自动重入队列
//   4. 500ms 消费超时: 允许线程周期性检查 stop_ 标志, 实现优雅关闭
//   5. RAII 管理: 析构函数自动 shutdown + join 所有线程
//
// 使用方式:
//   RabbitMQThreadPool pool("localhost", "sql_queue", 2, executeMysql);
//   pool.start();  // 启动 2 个 Worker 线程
//   // ... 服务运行 ...
//   pool.shutdown(); // 或依赖析构函数自动停止
// ============================================================================
class RabbitMQThreadPool {
public:
    // ── HandlerFunc: 消息处理回调签名 ──────────────────────────────────
    // 接收消息体 (string), 无返回值
    // 当前项目: executeMysql(const std::string sql)
    using HandlerFunc = std::function<void(const std::string&)>;

    // ── 构造函数 ───────────────────────────────────────────────────────
    // host       — RabbitMQ 服务器地址 ("localhost" 或环境变量)
    // queue      — 要消费的队列名 ("sql_queue")
    // thread_num — 消费者工作线程数 (当前 2)
    // handler    — 消息处理函数 (executeMysql → MysqlUtil::executeUpdate)
    RabbitMQThreadPool(const std::string& host,
        const std::string& queue,
        int thread_num,
        HandlerFunc handler)
        : stop_(false),
        rabbitmq_host_(host),
        queue_name_(queue),
        thread_num_(thread_num),
        handler_(handler) {}

    // ── 生命周期管理 ───────────────────────────────────────────────────
    void start();       // 启动 thread_num_ 个 Worker 线程
    void shutdown();    // 设置 stop_ 标志 → 等待所有线程退出 → join

    ~RabbitMQThreadPool() {
        shutdown();     // RAII: 析构时自动停止, 防止线程泄漏
    }

private:
    // ── worker: 消费者线程的主循环 ────────────────────────────────────
    // 每个 Worker: 创建独立 Channel → DeclareQueue → BasicConsume
    //             → while(!stop_) BasicConsumeMessage(500ms)
    //             → handler_(msg) → BasicAck
    void worker(int id);

    // ── 成员变量 ─────────────────────────────────────────────────────
    std::vector<std::thread> workers_;  // 工作线程向量
    std::atomic<bool> stop_;            // 停止信号 (原子: 跨线程即时可见)
    std::string queue_name_;            // 队列名 ("sql_queue")
    int thread_num_;                    // 线程数 (2)
    std::string rabbitmq_host_;         // RabbitMQ 服务地址
    HandlerFunc handler_;               // 消息处理回调
};
