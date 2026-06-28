#include "../../../include/utils/db/DbConnectionPool.h"
#include "../../../include/utils/db/DbException.h"
#include <muduo/base/Logging.h>

namespace http 
{
namespace db 
{

void DbConnectionPool::init(const std::string& host,
                          const std::string& user,
                          const std::string& password,
                          const std::string& database,
                          size_t poolSize) 
{
    // 连接池会被多个线程访问，所以操作其成员变量时需要加锁
    std::lock_guard<std::mutex> lock(mutex_);
    // 确保只初始化一次
    if (initialized_) 
    {
        return;
    }

    host_ = host;
    user_ = user;
    password_ = password;
    database_ = database;

    // 创建连接
    for (size_t i = 0; i < poolSize; ++i) 
    {
        connections_.push(createConnection());
    }

    initialized_ = true;
    LOG_INFO << "Database connection pool initialized with " << poolSize << " connections";
}

DbConnectionPool::DbConnectionPool() 
{
    checkThread_ = std::thread(&DbConnectionPool::checkConnections, this);
    checkThread_.detach();
}

DbConnectionPool::~DbConnectionPool() 
{
    std::lock_guard<std::mutex> lock(mutex_);
    while (!connections_.empty()) 
    {
        connections_.pop();
    }
    LOG_INFO << "Database connection pool destroyed";
}

// 修改获取连接的函数
std::shared_ptr<DbConnection> DbConnectionPool::getConnection() 
{
    std::shared_ptr<DbConnection> conn;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        
        while (connections_.empty()) 
        {
            if (!initialized_) 
            {
                throw DbException("Connection pool not initialized");
            }
            LOG_INFO << "Waiting for available connection...";
            cv_.wait(lock);
        }
        
        conn = connections_.front();
        connections_.pop();
    } // 释放锁
    
    try 
    {
        // 在锁外检查连接
        if (!conn->ping()) 
        {
            LOG_WARN << "Connection lost, attempting to reconnect...";
            conn->reconnect();
        }
        
        //构造“自动归还”的智能指针并返回
        // 返回一个共享指针，指向数据库连接。
        // 但这里并不是直接返回从队列中取出的 conn，而是构造了一个新的 shared_ptr，
        // 并为它指定了一个自定义的删除器。目的：当业务代码不再使用该连接时，
        // 连接不会真的被销毁，而是自动归还到连接池中。
        return std::shared_ptr<DbConnection>(
            conn.get(),                     // 被管理的原始指针，指向同一个 DbConnection 对象
            [this, conn](DbConnection*) {   // 自定义删除器（lambda 表达式）
                                            // 参数 DbConnection* 没有名字，因为函数体内不需要它
                                            // （我们要归还的是 conn 这个 shared_ptr，不是原始指针）
                std::lock_guard<std::mutex> lock(mutex_); // 加锁，保证线程安全
                connections_.push(conn);                  // 将连接放回连接池队列
                cv_.notify_one();                         // 唤醒一个正在等待连接的线程
                });
    } 
    catch (const std::exception& e) 
    {
        LOG_ERROR << "Failed to get connection: " << e.what();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connections_.push(conn);
            cv_.notify_one();
        }
        throw;
    }
}

std::shared_ptr<DbConnection> DbConnectionPool::createConnection() 
{
    return std::make_shared<DbConnection>(host_, user_, password_, database_);
}

// 修改检查连接的函数
void DbConnectionPool::checkConnections() 
{
    // 独立线程的无限循环，持续监控连接池健康状态
    while (true) 
    {
        try 
        {
            // 用于暂存当前所有空闲连接的局部容器
            std::vector<std::shared_ptr<DbConnection>> connsToCheck;
            {
                // 加锁，保护连接池队列
                std::unique_lock<std::mutex> lock(mutex_);
                if (connections_.empty()) 
                {
                    // 池中无空闲连接，休眠1秒后继续循环，避免空转
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }
                
                // 复制整个队列（queue 不支持遍历，通过复制底层容器实现）
                auto temp = connections_;
                while (!temp.empty()) 
                {
                    connsToCheck.push_back(temp.front());
                    temp.pop();
                }
            } // 离开作用域，互斥锁自动释放，后续网络操作不会阻塞其他线程

            // 在锁外对所有空闲连接进行健康检查
            for (auto& conn : connsToCheck) 
            {
                // 轻量心跳检测：SELECT 1
                if (!conn->ping()) 
                {
                    try 
                    {
                        // 连接失效，尝试自动重连
                        conn->reconnect();
                    } 
                    catch (const std::exception& e) 
                    {
                        // 重连失败（如数据库服务器宕机），记录错误并继续检查下一个连接
                        LOG_ERROR << "Failed to reconnect: " << e.what();
                    }
                }
            }
            
            // 完成一轮检查后休眠 60 秒，避免频繁检测消耗资源
            std::this_thread::sleep_for(std::chrono::seconds(60));
        } 
        catch (const std::exception& e) 
        {
            // 兜底异常保护，确保检查线程不会因任何意外退出
            LOG_ERROR << "Error in check thread: " << e.what();
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
}

} // namespace db
} // namespace http