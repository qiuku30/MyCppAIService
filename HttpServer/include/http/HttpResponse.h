#pragma once

#include <muduo/net/TcpServer.h>

namespace http
{

class HttpServer;
namespace router { class Router; }

class HttpResponse 
{
public:
    enum HttpStatusCode
    {
        kUnknown,
        k200Ok = 200,
        k204NoContent = 204,
        k301MovedPermanently = 301,
        k400BadRequest = 400,
        k401Unauthorized = 401,
        k403Forbidden = 403,
        k404NotFound = 404,
        k409Conflict = 409,
        k413PayloadTooLarge = 413,
        k500InternalServerError = 500,
        k503ServiceUnavailable = 503,
    };

    HttpResponse(bool close = true)
        : httpVersion_("HTTP/1.1")
        , statusCode_(k500InternalServerError)
        , statusMessage_("Internal Server Error")
        , closeConnection_(close)
    {}

    void setVersion(std::string version)
    { httpVersion_ = version; }
    void setStatusCode(HttpStatusCode code);

    HttpStatusCode getStatusCode() const
    { return statusCode_; }

    void setStatusMessage(const std::string message)
    { statusMessage_ = message; }

    void setCloseConnection(bool on)
    { closeConnection_ = on; }

    bool closeConnection() const
    { return closeConnection_; }
    
    void setContentType(const std::string& contentType)
    { addHeader("Content-Type", contentType); }

    void setContentLength(uint64_t length)
    { addHeader("Content-Length", std::to_string(length)); }

    void addHeader(const std::string& key, const std::string& value)
    { headers_[key] = value; }
    
    void setBody(const std::string& body)
    {
        body_ = body;
        // body_ += "\0";
    }

    // 流式响应标记：设为 true 时 onRequest() 跳过序列化和 send()
    void setStreaming(bool v) { streaming_ = v; }
    bool isStreaming() const { return streaming_; }

    // SSE 流式响应快捷设置：Content-Type + Cache-Control + keep-alive
    void setupSSEHeaders()
    {
        setContentType("text/event-stream");
        addHeader("Cache-Control", "no-cache");
        addHeader("Connection", "keep-alive");
        setCloseConnection(false);
        setStreaming(true);
    }

    void setStatusLine(const std::string& version,
                         HttpStatusCode statusCode,
                         const std::string& statusMessage);

    void setErrorHeader(){}

    void appendToBuffer(muduo::net::Buffer* outputBuf) const;
private:
    friend class HttpServer;
    friend class router::Router;
    // Only dispatch can set labels; handlers must not use request data here.
    // This metadata is never serialized onto the response wire.
    void setLogRoute(const std::string& route) { logRoute_ = route; }
    const std::string& logRoute() const { return logRoute_; }

    std::string                        httpVersion_; 
    HttpStatusCode                     statusCode_;
    std::string                        statusMessage_;
    bool                               closeConnection_;
    std::map<std::string, std::string> headers_;
    std::string                        body_;
    std::string                        logRoute_ = "callback";
    bool                               isFile_ = false;
    bool                               streaming_ = false;  // SSE 流式标记
};

} // namespace http