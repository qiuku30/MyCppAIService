#pragma once
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
namespace muduo {
inline std::mutex capturedLogMutex;
inline std::vector<std::string> capturedLogs;
struct TestLog {
    std::ostringstream message;
    ~TestLog() { std::lock_guard<std::mutex> lock(capturedLogMutex); capturedLogs.push_back(message.str()); }
    template<class T> TestLog& operator<<(const T& value) { message << value; return *this; }
};
inline std::vector<std::string> takeLogs() {
    std::lock_guard<std::mutex> lock(capturedLogMutex);
    auto result = capturedLogs; capturedLogs.clear(); return result;
}
}
#define LOG_DEBUG muduo::TestLog()
#define LOG_INFO muduo::TestLog()
#define LOG_WARN muduo::TestLog()
#define LOG_ERROR muduo::TestLog()
