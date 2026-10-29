#pragma once
namespace muduo {
struct TestLog {
    template <typename T> TestLog& operator<<(const T&) { return *this; }
};
}
#define LOG_DEBUG muduo::TestLog()
#define LOG_INFO muduo::TestLog()
#define LOG_WARN muduo::TestLog()
#define LOG_ERROR muduo::TestLog()
