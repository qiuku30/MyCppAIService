#pragma once
#include <boost/any.hpp>
#include <muduo/base/Timestamp.h>
#include <memory>
#include <string>
#include <muduo/net/Buffer.h>

namespace muduo { namespace net {
class TcpConnection {
public:
    bool connected() const { return !shutdownCalled; }
    void setContext(const boost::any& value) { context_ = value; }
    boost::any* getMutableContext() { return &context_; }
    void send(const std::string& text) { sent += text; }
    void send(Buffer* buffer) { sent += buffer->retrieveAllAsString(); }
    void shutdown() { shutdownCalled = true; }
    std::string sent;
    bool shutdownCalled = false;
private:
    boost::any context_;
};
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
}}
