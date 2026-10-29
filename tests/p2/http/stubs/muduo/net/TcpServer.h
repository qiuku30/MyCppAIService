#pragma once
#include <functional>
#include <string>
#include <muduo/base/Timestamp.h>
#include <muduo/net/TcpConnection.h>
#include <muduo/net/EventLoop.h>

namespace muduo { namespace net {
class InetAddress { public: explicit InetAddress(int) {} };
class TcpServer {
public:
    enum Option { kNoReusePort, kReusePort };
    using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
    using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*, muduo::Timestamp)>;
    inline static TcpServer* latest = nullptr;
    inline static unsigned constructionViolations = 0;
    inline static unsigned destructionViolations = 0;
    TcpServer(EventLoop* loop, const InetAddress&, const std::string& name, Option)
        : loop_(loop), name_(name) {
        latest = this;
        if (!EventLoop::liveLoops.count(loop_)) ++constructionViolations;
    }
    ~TcpServer() {
        if (!EventLoop::liveLoops.count(loop_)) ++destructionViolations;
        latest = nullptr;
    }
    void setThreadNum(int) {}
    void start() {}
    EventLoop* getLoop() const { return loop_; }
    std::string name() const { return name_; }
    std::string ipPort() const { return "fixture"; }
    void setConnectionCallback(ConnectionCallback cb) { connectionCallback_ = std::move(cb); }
    void setMessageCallback(MessageCallback cb) { messageCallback_ = std::move(cb); }
    void connect(const TcpConnectionPtr& conn) { connectionCallback_(conn); }
    void receive(const TcpConnectionPtr& conn, Buffer* buf) { messageCallback_(conn, buf, muduo::Timestamp()); }
private:
    EventLoop* loop_;
    std::string name_;
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
};
}}
