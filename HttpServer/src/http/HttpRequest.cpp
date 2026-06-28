#include "../../include/http/HttpRequest.h"

namespace http
{

void HttpRequest::setReceiveTime(muduo::Timestamp t)
{
    receiveTime_ = t;
}

//解析 HTTP 请求方法字符串，转为枚举值存储
bool HttpRequest::setMethod(const char *start, const char *end)
{
    assert(method_ == kInvalid);
    std::string m(start, end); // [start, end)
    if (m == "GET")
    {
        method_ = kGet;
    }
    else if (m == "POST")
    {
        method_ = kPost;
    }
    else if (m == "PUT")
    {
        method_ = kPut;
    }
    else if (m == "DELETE")
    {
        method_ = kDelete;
    }
    else if (m == "OPTIONS")
    {
        method_ = kOptions;
    }
    else
    {
        method_ = kInvalid;
    }

    return method_ != kInvalid;
}

//截取一段内存 → 变成路径字符串 → 存进 path_
void HttpRequest::setPath(const char *start, const char *end)
{
    path_.assign(start, end);
}

//存储从路径中提取出来的路径参数（键值对）
void HttpRequest::setPathParameters(const std::string &key, const std::string &value)
{
    pathParameters_[key] = value;
}

//根据键名查找某个路径参数的值
std::string HttpRequest::getPathParameters(const std::string &key) const
{
    auto it = pathParameters_.find(key);
    if (it != pathParameters_.end())
    {
        return it->second;
    }
    return "";
}

//根据键名查找某个查询参数的值
std::string HttpRequest::getQueryParameters(const std::string &key) const
{
    auto it = queryParameters_.find(key);
    if (it != queryParameters_.end())
    {
        return it->second;
    }
    return "";
}

// 这是从问号后面分割参数
void HttpRequest::setQueryParameters(const char *start, const char *end)
{
    std::string argumentStr(start, end);
    std::string::size_type pos = 0;
    std::string::size_type prev = 0;

    // 按 & 分割多个参数
    while ((pos = argumentStr.find('&', prev)) != std::string::npos)
    {
        //一个完整的 key=value 对（不含 &）
        std::string pair = argumentStr.substr(prev, pos - prev);
        //在 pair 字符串中查找字符 '='，返回该字符的索引
        std::string::size_type equalPos = pair.find('=');

        //检查是否找到了 '='。只有当 pair 中确实包含等号，才是合法的键值对
        if (equalPos != std::string::npos)
        {
            //截取 pair 从索引 0 开始、长度为 equalPos 的子串，即等号之前的部分，作为键
            std::string key = pair.substr(0, equalPos);
            //截取从 equalPos + 1 位置开始直到字符串末尾的子串，即等号之后的部分，作为值
            std::string value = pair.substr(equalPos + 1);
            queryParameters_[key] = value;
        }

        prev = pos + 1;
    }

    // 处理最后一个参数
    std::string lastPair = argumentStr.substr(prev);
    std::string::size_type equalPos = lastPair.find('=');
    if (equalPos != std::string::npos)
    {
        std::string key = lastPair.substr(0, equalPos);
        std::string value = lastPair.substr(equalPos + 1);
        queryParameters_[key] = value;
    }
}

//用于解析一个 HTTP 头部行，并将键值对存入 headers_ 映射中
void HttpRequest::addHeader(const char *start, const char *colon, const char *end)
{
    std::string key(start, colon);
    ++colon;
    while (colon < end && isspace(*colon))
    {
        ++colon;
    }
    std::string value(colon, end);
    while (!value.empty() && isspace(value[value.size() - 1])) // 消除尾部空格
    {
        value.resize(value.size() - 1);
    }
    headers_[key] = value;
}

std::string HttpRequest::getHeader(const std::string &field) const
{
    std::string result;
    auto it = headers_.find(field);
    if (it != headers_.end())
    {
        result = it->second;
    }
    return result;
}

void HttpRequest::swap(HttpRequest &that)
{
    std::swap(method_, that.method_);
    std::swap(path_, that.path_);
    std::swap(pathParameters_, that.pathParameters_);
    std::swap(queryParameters_, that.queryParameters_);
    std::swap(version_, that.version_);
    std::swap(headers_, that.headers_);
    std::swap(receiveTime_, that.receiveTime_);
}

} // namespace http