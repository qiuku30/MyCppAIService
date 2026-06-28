#pragma once 

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <unordered_map>

#include <muduo/net/TcpServer.h>
#include <muduo/net/EventLoop.h>
#include <muduo/base/Logging.h>

#include "HttpContext.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "../router/Router.h"
#include "../session/SessionManager.h"
#include "../middleware/MiddlewareChain.h"
#include "../middleware/cors/CorsMiddleware.h"
#include "../ssl/SslConnection.h"
#include "../ssl/SslContext.h"

class HttpRequest;
class HttpResponse;

namespace http
{

class HttpServer : muduo::noncopyable
{
public:
    using HttpCallback = std::function<void (const http::HttpRequest&, http::HttpResponse*)>;
    
    // 构造函数
    HttpServer(int port,   //监听端口号
               const std::string& name,  //服务器名称（用于 muduo 内部日志
               bool useSSL = false,    //是否启用 SSL/TLS
               muduo::net::TcpServer::Option option = muduo::net::TcpServer::kNoReusePort);  //端口复用选项，默认不复用端口
    
     //设置 muduo 网络库 IO 工作线程池数量
    void setThreadNum(int numThreads)
    {
        server_.setThreadNum(numThreads);
    }

    void start();

    //获取服务器底层的主事件循环对象指针
    muduo::net::EventLoop* getLoop() const 
    { 
        return server_.getLoop(); 
    }

    //设置一个默认的请求处理回调，当路由匹配失败时可能被调用
    void setHttpCallback(const HttpCallback& cb)
    {
        httpCallback_ = cb;
    }

    // 注册静态路由处理器
    void Get(const std::string& path, const HttpCallback& cb)
    {
        router_.registerCallback(HttpRequest::kGet, path, cb);
    }
    
    // 注册静态路由处理器
    void Get(const std::string& path, router::Router::HandlerPtr handler)
    {
        router_.registerHandler(HttpRequest::kGet, path, handler);
    }

    void Post(const std::string& path, const HttpCallback& cb)
    {
        router_.registerCallback(HttpRequest::kPost, path, cb);
    }

    void Post(const std::string& path, router::Router::HandlerPtr handler)
    {
        router_.registerHandler(HttpRequest::kPost, path, handler);
    }

    // 注册动态路由处理器
    void addRoute(HttpRequest::Method method, const std::string& path, router::Router::HandlerPtr handler)
    {
        router_.addRegexHandler(method, path, handler);
    }

    // 注册动态路由处理函数
    void addRoute(HttpRequest::Method method, const std::string& path, const router::Router::HandlerCallback& callback)
    {
        router_.addRegexCallback(method, path, callback);
    }

    // 设置会话管理器
    void setSessionManager(std::unique_ptr<session::SessionManager> manager)
    {
        sessionManager_ = std::move(manager);
    }

    // 获取会话管理器
    session::SessionManager* getSessionManager() const
    {
        return sessionManager_.get();
    }

    // 添加中间件的方法
    void addMiddleware(std::shared_ptr<middleware::Middleware> middleware) 
    {
        middlewareChain_.addMiddleware(middleware);
    }

    void enableSSL(bool enable) 
    {
        useSSL_ = enable;
    }

    void setSslConfig(const ssl::SslConfig& config);

private:

    //给底层 muduo TcpServer 注册两个关键回调：
    //有客户端连接/断开 → 调用 onConnection
    //有客户端发数据 → 调用 onMessage
    void initialize();

    //muduo 底层 TCP 连接回调  客户端连接成功 或 断开连接时自动调用
    void onConnection(const muduo::net::TcpConnectionPtr& conn);

    //muduo 底层 TCP 数据回调  收到客户端发送的原始数据时调用
    void onMessage(const muduo::net::TcpConnectionPtr& conn,
                   muduo::net::Buffer* buf,
                   muduo::Timestamp receiveTime);

    //HTTP 请求处理入口函数
    void onRequest(const muduo::net::TcpConnectionPtr&, const HttpRequest&);

    //HTTP 业务核心处理函数
    void handleRequest(const HttpRequest& req, HttpResponse* resp);
    
private:
    muduo::net::InetAddress                      listenAddr_; // 监听地址（ip地址和端口号）
    muduo::net::TcpServer                        server_;  //管理 TCP 连接的接受、关闭、数据收发和事件分发
    muduo::net::EventLoop                        mainLoop_; // 主循环
    HttpCallback                                 httpCallback_; // 回调函数
    router::Router                               router_; // 路由表，存储注册的路径与处理函数的映射
    std::unique_ptr<session::SessionManager>     sessionManager_; // 会话管理器
    middleware::MiddlewareChain                  middlewareChain_; // 中间件链
    std::unique_ptr<ssl::SslContext>             sslCtx_; // SSL 上下文
    bool                                         useSSL_; // 是否使用 SSL   
    // TcpConnectionPtr -> SslConnectionPtr 
    std::map<muduo::net::TcpConnectionPtr, std::unique_ptr<ssl::SslConnection>> sslConns_;
}; 

} // namespace http