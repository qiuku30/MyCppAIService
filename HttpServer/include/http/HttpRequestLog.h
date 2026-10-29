#pragma once
#include "HttpRequest.h"
#include "HttpResponse.h"
#include <muduo/base/Logging.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <sstream>

namespace http { namespace request_log {
using Clock = std::chrono::steady_clock;
inline uint64_t nextId()
{
    static std::atomic<uint64_t> sequence{0};
    return sequence.fetch_add(1, std::memory_order_relaxed) + 1;
}
inline const char* methodName(HttpRequest::Method method)
{
    switch (method) {
    case HttpRequest::kGet: return "GET";
    case HttpRequest::kPost: return "POST";
    case HttpRequest::kHead: return "HEAD";
    case HttpRequest::kPut: return "PUT";
    case HttpRequest::kDelete: return "DELETE";
    case HttpRequest::kOptions: return "OPTIONS";
    default: return "INVALID";
    }
}
// Only registered templates or fixed categories belong here. Quote and bound
// even configuration supplied labels so one request always remains one line.
inline std::string quoteLabel(const std::string& label)
{
    std::string result = "\"";
    const char hex[] = "0123456789abcdef";
    const size_t limit = 128;
    for (size_t i = 0; i < label.size() && i < limit; ++i) {
        const auto ch = static_cast<unsigned char>(label[i]);
        if (ch == '"' || ch == '\\') { result += '\\'; result += static_cast<char>(ch); }
        else if (ch < 0x20 || ch >= 0x7f) { result += "\\x"; result += hex[ch >> 4]; result += hex[ch & 15]; }
        else result += static_cast<char>(ch);
    }
    if (label.size() > limit) result += "...";
    return result + '"';
}
inline void record(uint64_t id, HttpRequest::Method method, const std::string& route,
                   int status, Clock::time_point started, size_t bytes, bool streaming = false)
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();
    std::ostringstream event;
    event << "http_request request_id=" << id << " method=" << methodName(method)
          << " route=" << quoteLabel(route) << " status=" << status
          << " elapsed_us=" << elapsed << " event=" << (streaming ? "stream_open" : "response");
    if (!streaming) event << " response_bytes=" << bytes;
    LOG_INFO << event.str();
}
}} // namespace http::request_log
