#include "../../include/http/HttpServer.h"
#include "../../include/http/HttpRequestLog.h"

#include <any>
#include <functional>
#include <memory>

namespace http
{

// 默认http回应函数
void defaultHttpCallback(const HttpRequest &req, HttpResponse *resp)
{
    resp->setStatusLine(req.getVersion(), HttpResponse::k404NotFound, "Not Found");
    resp->setCloseConnection(true);
}

HttpServer::HttpServer(int port,
                       const std::string &name,
                       bool useSSL,
                       muduo::net::TcpServer::Option option)
    : listenAddr_(port)
    , mainLoop_()
    , server_(&mainLoop_, listenAddr_, name, option)
    , httpCallback_(std::bind(&HttpServer::handleRequest, this, std::placeholders::_1, std::placeholders::_2))
    , useSSL_(useSSL)
{
    initialize();
}

// 服务器运行函数
void HttpServer::start()
{
    //打印启动日志
    LOG_WARN << "HttpServer[" << server_.name() << "] starts listening on" << server_.ipPort();
    server_.start();  //开始监听端口，接收新连接
    mainLoop_.loop();  //进入事件循环，阻塞当前线程直到服务器停止
}

//将底层网络事件（连接、数据）与上层 HTTP 处理逻辑对接起来
void HttpServer::initialize()
{
    // 设置回调函数
    //连接回调  bind 把成员函数 HttpServer::onConnection 适配成这个原型
    server_.setConnectionCallback(
        std::bind(&HttpServer::onConnection, this, std::placeholders::_1));  //占位const muduo::net::TcpConnectionPtr&

    //消息回调
    server_.setMessageCallback(
        std::bind(&HttpServer::onMessage, this,
                  std::placeholders::_1,  //const muduo::net::TcpConnectionPtr&
                  std::placeholders::_2,  //muduo::net::Buffer*
                  std::placeholders::_3)); //muduo::Timestamp
}

//在服务器启动前一次性加载 SSL 证书和私钥，并初始化可供所有加密连接复用的 SSL 上下文，失败则直接终止程序
void HttpServer::setSslConfig(const ssl::SslConfig& config)
{
    if (useSSL_)
    {
        sslCtx_ = std::make_unique<ssl::SslContext>(config);
        if (!sslCtx_->initialize())
        {
            LOG_ERROR << "Failed to initialize SSL context";
            abort();
        }
    }
}

void HttpServer::onConnection(const muduo::net::TcpConnectionPtr& conn)
{
    if (conn->connected())
    {
        // 如果开启了 HTTPS 加密
        if (useSSL_)
        {
            //创建一个 SSL 加密连接
            auto sslConn = std::make_unique<ssl::SslConnection>(conn, sslCtx_.get());

            //加密通道收到数据 → 调用 onMessage
            sslConn->setMessageCallback(
                std::bind(&HttpServer::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
            //把这个加密通道存起来，方便以后用
            sslConns_[conn] = std::move(sslConn);
            //开始 SSL 握手（建立加密通道）
            sslConns_[conn]->startHandshake();
        }
        //初始化 HTTP 解析上下文
        conn->setContext(HttpContext());
    }
    else 
    {
        if (useSSL_)
        {
            sslConns_.erase(conn);
        }
    }
}

void HttpServer::onMessage(const muduo::net::TcpConnectionPtr &conn,
                           muduo::net::Buffer *buf,
                           muduo::Timestamp receiveTime)
{
    HttpContext* context = nullptr;
    try
    {
        // 这层判断只是代表是否支持ssl
        if (useSSL_)
        {
            LOG_INFO << "onMessage useSSL_ is true";
            // 1.查找对应的SSL连接
            auto it = sslConns_.find(conn);
            if (it != sslConns_.end())
            {
                LOG_INFO << "onMessage sslConns_ is not empty";
                // 2. SSL连接处理数据
                it->second->onRead(conn, buf, receiveTime);

                // 3. 如果 SSL 握手还未完成，直接返回
                if (!it->second->isHandshakeCompleted())
                {
                    LOG_INFO << "onMessage sslConns_ is not empty";
                    return;
                }

                // 4. 从SSL连接的解密缓冲区获取数据
                muduo::net::Buffer* decryptedBuf = it->second->getDecryptedBuffer();
                if (decryptedBuf->readableBytes() == 0)
                    return; // 没有解密后的数据

                // 5. 使用解密后的数据进行HTTP 处理
                buf = decryptedBuf; // 将 buf 指向解密后的数据
                LOG_INFO << "onMessage decryptedBuf is not empty";
            }
        }
        // HttpContext对象用于解析出buf中的请求报文，并把报文的关键信息封装到HttpRequest对象中
        context = boost::any_cast<HttpContext>(conn->getMutableContext());
        if (context->processingStopped())
        {
            buf->retrieve(buf->readableBytes());
            return;
        }
        while (conn->connected() && buf->readableBytes() > 0)
        {
            if (!context->parseRequest(buf, receiveTime))
            {
                context->stopProcessing();
                buf->retrieve(buf->readableBytes());
                conn->send("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
                conn->shutdown();
                return;
            }
            // A partial request must wait for the next message callback.
            if (!context->gotAll()) return;

            const bool canContinue = onRequest(conn, context->request());
            context->reset();
            if (!canContinue)
            {
                context->stopProcessing();
                buf->retrieve(buf->readableBytes());
                return;
            }
        }
        if (!conn->connected())
        {
            context->stopProcessing();
            buf->retrieve(buf->readableBytes());
        }
    }
    catch (const std::exception &)
    {
        // 捕获异常，返回错误信息
        if (context) context->stopProcessing();
        buf->retrieve(buf->readableBytes());
        LOG_ERROR << "http_error category=message_exception";
        conn->send("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
        conn->shutdown();
    }
}

//将一个完整的 HTTP 请求交由业务逻辑处理，生成响应，发送给客户端，并决定是否关闭连接
bool HttpServer::onRequest(const muduo::net::TcpConnectionPtr &conn, const HttpRequest &req)
{
    const auto started = request_log::Clock::now();
    const auto requestId = request_log::nextId();
    const std::string connection = req.getHeader("Connection");
    const auto hasConnectionToken = [&connection](const std::string& expected)
    {
        size_t begin = 0;
        while (begin < connection.size())
        {
            const size_t comma = connection.find(',', begin);
            std::string token = connection.substr(begin, comma == std::string::npos ? comma : comma - begin);
            const size_t first = token.find_first_not_of(" \t");
            if (first != std::string::npos)
            {
                token = token.substr(first, token.find_last_not_of(" \t") - first + 1);
                for (char& ch : token)
                {
                    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
                }
                if (token == expected) return true;
            }
            if (comma == std::string::npos) break;
            begin = comma + 1;
        }
        return false;
    };
    const bool close = hasConnectionToken("close") ||
                       (req.getVersion() == "HTTP/1.0" && !hasConnectionToken("keep-alive"));
    HttpResponse response(close);
    response.setVersion(req.getVersion());

    // 注入 TcpConnection 供 Handler 流式发送 SSE 数据
    const_cast<HttpRequest&>(req).setConnection(conn);

    // 根据请求报文信息来封装响应报文对象
    try {
        httpCallback_(req, &response);
    } catch (const std::exception&) {
        // onMessage retains the existing fixed 400 response for callback exceptions.
        constexpr size_t errorBytes = sizeof("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n") - 1;
        request_log::record(requestId, req.method(), "error", 400, started, errorBytes);
        throw;
    }

    // 流式响应：Handler 已接管连接，跳过序列化和 send
    if (response.isStreaming()) {
        request_log::record(requestId, req.method(), response.logRoute(), response.getStatusCode(), started, 0, true);
        return false;
    }

    // A request's close token cannot be overridden by a normal handler.
    if (close) response.setCloseConnection(true);

    // 可以给response设置一个成员，判断是否请求的是文件，如果是文件设置为true，并且存在文件位置在这里send出去。
    muduo::net::Buffer buf;
    response.appendToBuffer(&buf);
    request_log::record(requestId, req.method(), response.logRoute(), response.getStatusCode(), started, buf.readableBytes());

    conn->send(&buf);
    // 如果是短连接的话，返回响应报文后就断开连接
    if (response.closeConnection())
    {
        conn->shutdown();
        return false;
    }
    return conn->connected();
}

// 执行请求对应的路由处理函数
void HttpServer::handleRequest(const HttpRequest &req, HttpResponse *resp)
{
    try
    {
        // 处理请求前的中间件
        HttpRequest mutableReq = req;
        middlewareChain_.processBefore(mutableReq);

        // 路由处理
        if (!router_.route(mutableReq, resp))
        {
            resp->setLogRoute("unmatched");
            resp->setStatusLine(req.getVersion(), HttpResponse::k404NotFound, "Not Found");
            resp->setCloseConnection(true);
        }

        // 处理响应后的中间件
        middlewareChain_.processAfter(*resp);
    }
    catch (const HttpResponse& res) 
    {
        // 处理中间件抛出的响应（如CORS预检请求）
        *resp = res;
        resp->setLogRoute(req.method() == HttpRequest::kOptions ? "preflight" : "callback");
        resp->setVersion(req.getVersion());
    }
    catch (const std::exception& e) 
    {
        // 错误处理
        *resp = HttpResponse(true);
        resp->setLogRoute("error");
        LOG_ERROR << "http_error category=handler_exception";
        resp->setStatusLine(req.getVersion(), HttpResponse::k500InternalServerError, "Internal Server Error");
        resp->setBody(e.what());
    }
}

} // namespace http