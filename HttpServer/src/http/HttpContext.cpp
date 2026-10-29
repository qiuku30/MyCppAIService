#include "../../include/http/HttpContext.h"

using namespace muduo;
using namespace muduo::net;

namespace http
{

// 将报文解析出来将关键信息封装到HttpRequest对象里面去
bool HttpContext::parseRequest(Buffer *buf, Timestamp receiveTime)
{
    bool ok = true; // 解析每行请求格式是否正确
    bool hasMore = true;// 控制循环是否继续（即是否还有数据可处理/需要处理）
    //读取并解析 HTTP 第一行（请求行）
    while (hasMore)
    {
        if (state_ == kExpectRequestLine)
        {
            const char *crlf = buf->findCRLF(); // 注意这个返回值边界可能有错
            if (crlf)
            {
                ok = processRequestLine(buf->peek(), crlf);
                if (ok)
                {
                    request_.setReceiveTime(receiveTime);
                    buf->retrieveUntil(crlf + 2);
                    state_ = kExpectHeaders;
                }
                else
                {
                    hasMore = false;
                }
            }
            else
            {
                hasMore = false;
            }
        }
        //解析头部行
        else if (state_ == kExpectHeaders)
        {
            const char *crlf = buf->findCRLF();
            if (crlf)
            {
                //找到一个冒号，且冒号在 CRLF 之前 → 合法的头部行
                const char *colon = std::find(buf->peek(), crlf, ':');
                if (colon < crlf)
                {
                    if (!request_.addHeader(buf->peek(), colon, crlf)) return false;
                }
                else if (buf->peek() == crlf)
                {
                    // Transfer codings are not decoded by this server. Reject them rather
                    // than leave their bytes to be parsed as a subsequent request.
                    if (request_.headers().count("transfer-encoding")) return false;

                    auto contentLength = request_.headers().find("content-length");
                    if (contentLength != request_.headers().end())
                    {
                        if (contentLength->second.empty()) return false;
                        constexpr uint64_t maxBodyLength = 10 * 1024 * 1024;
                        uint64_t length = 0;
                        for (unsigned char ch : contentLength->second)
                        {
                            if (ch < '0' || ch > '9') return false;
                            const unsigned digit = ch - '0';
                            if (length > (maxBodyLength - digit) / 10) return false;
                            length = length * 10 + digit;
                        }
                        request_.setContentLength(length);
                        state_ = length > 0 ? kExpectBody : kGotAll;
                        hasMore = length > 0;
                    }
                    else
                    {
                        // Preserve the application's requirement for explicit POST/PUT length.
                        if (request_.method() == HttpRequest::kPost ||
                            request_.method() == HttpRequest::kPut) return false;
                        state_ = kGotAll;
                        hasMore = false;
                    }
                }
                else
                {
                    ok = false; // Header行格式错误
                    hasMore = false;
                }
                buf->retrieveUntil(crlf + 2); // 开始读指针指向下一行数据
            }
            else
            {
                hasMore = false;
            }
        }
        else if (state_ == kExpectBody)
        {
            // 检查缓冲区中是否有足够的数据
            if (buf->readableBytes() < request_.contentLength())
            {
                hasMore = false; // 数据不完整，等待更多数据
                return true;
            }

            // 只读取 Content-Length 指定的长度
            std::string body(buf->peek(), buf->peek() + request_.contentLength());
            request_.setBody(body);

            // 准确移动读指针
            buf->retrieve(request_.contentLength());

            state_ = kGotAll;
            hasMore = false;
        }
    }
    return ok; // ok为false代表报文语法解析错误
}

// 解析请求行
bool HttpContext::processRequestLine(const char *begin, const char *end)
{
    bool succeed = false;
    const char *start = begin;
    const char *space = std::find(start, end, ' ');//在 [start, end) 区间内查找第一个空格

    if (space != end && request_.setMethod(start, space))   
    {
        start = space + 1;
        space = std::find(start, end, ' ');  //查找第二个空格
        if (space != end)
        {
            const char *argumentStart = std::find(start, space, '?');
            if (argumentStart != space) // 请求带参数
            {
                request_.setPath(start, argumentStart); // 问号之前是纯粹路径
                request_.setQueryParameters(argumentStart + 1, space);  //问号之后直到空格之前是查询字符串
            }
            else // 请求不带参数
            {
                request_.setPath(start, space);
            }

            start = space + 1;  //移动到版本字符串的起始位置
            succeed = ((end - start == 8) && std::equal(start, end - 1, "HTTP/1."));
            if (succeed)
            {
                if (*(end - 1) == '1')
                {
                    request_.setVersion("HTTP/1.1");
                }
                else if (*(end - 1) == '0')
                {
                    request_.setVersion("HTTP/1.0");
                }
                else
                {
                    succeed = false;
                }
            }
        }
    }
    return succeed;
}

} // namespace http