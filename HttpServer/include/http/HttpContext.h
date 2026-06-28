#pragma once

#include <iostream>

#include <muduo/net/TcpServer.h>

#include "HttpRequest.h"

namespace http
{

class HttpContext 
{
public:
    enum HttpRequestParseState
    {
        kExpectRequestLine, // 解析请求行
        kExpectHeaders, // 解析请求头
        kExpectBody, // 解析请求体
        kGotAll, // 解析完成
    };
    
    HttpContext()
    : state_(kExpectRequestLine)
    {}

    bool parseRequest(muduo::net::Buffer* buf, muduo::Timestamp receiveTime);
    bool gotAll() const 
    { return state_ == kGotAll;  }

    void reset()
    {
        state_ = kExpectRequestLine;
        HttpRequest dummyData;
        request_.swap(dummyData);
    }

    const HttpRequest& request() const
    { return request_;}

    HttpRequest& request()
    { return request_;}

private:
    bool processRequestLine(const char* begin, const char* end);
private:
    HttpRequestParseState state_;// 解析状态：当前解析到哪一步
    HttpRequest           request_;// 存储解析结果：最终生成的请求对象
};

} // namespace http