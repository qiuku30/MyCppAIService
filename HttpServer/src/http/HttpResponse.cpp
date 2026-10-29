#include "../../include/http/HttpResponse.h"

namespace http
{

void HttpResponse::setStatusCode(HttpStatusCode code)
{
    statusCode_ = code;
    switch (code)
    {
    case k200Ok: statusMessage_ = "OK"; break;
    case k204NoContent: statusMessage_ = "No Content"; break;
    case k301MovedPermanently: statusMessage_ = "Moved Permanently"; break;
    case k400BadRequest: statusMessage_ = "Bad Request"; break;
    case k401Unauthorized: statusMessage_ = "Unauthorized"; break;
    case k403Forbidden: statusMessage_ = "Forbidden"; break;
    case k404NotFound: statusMessage_ = "Not Found"; break;
    case k409Conflict: statusMessage_ = "Conflict"; break;
    case k413PayloadTooLarge: statusMessage_ = "Payload Too Large"; break;
    case k500InternalServerError: statusMessage_ = "Internal Server Error"; break;
    case k503ServiceUnavailable: statusMessage_ = "Service Unavailable"; break;
    default:
        statusCode_ = k500InternalServerError;
        statusMessage_ = "Internal Server Error";
        break;
    }
}

void HttpResponse::appendToBuffer(muduo::net::Buffer* outputBuf) const
{
    outputBuf->append(httpVersion_ + " " + std::to_string(statusCode_) + " " + statusMessage_ + "\r\n");
    outputBuf->append(closeConnection_ ? "Connection: close\r\n" : "Connection: Keep-Alive\r\n");

    const bool noBody = (statusCode_ >= 100 && statusCode_ < 200) ||
                        statusCode_ == k204NoContent || statusCode_ == 304;
    if (!streaming_ && !noBody)
    {
        outputBuf->append("Content-Length: " + std::to_string(body_.size()) + "\r\n");
    }
    for (const auto& header : headers_)
    {
        std::string key = header.first;
        for (char& ch : key)
        {
            if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        }
        if (key == "connection") continue;
        if ((!streaming_ || noBody) && (key == "content-length" || key == "transfer-encoding")) continue;
        outputBuf->append(header.first);
        outputBuf->append(": ");
        outputBuf->append(header.second);
        outputBuf->append("\r\n");
    }
    outputBuf->append("\r\n");
    if (!streaming_ && !noBody) outputBuf->append(body_);
}

void HttpResponse::setStatusLine(const std::string& version,
                                 HttpStatusCode statusCode,
                                 const std::string& statusMessage)
{
    httpVersion_ = version;
    statusCode_ = statusCode;
    statusMessage_ = statusMessage;
}

} // namespace http