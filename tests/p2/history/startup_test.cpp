#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace muduo { struct Logger { enum LogLevel { WARN }; static void setLogLevel(LogLevel) {} }; }
#define LOG_INFO std::cerr
#define LOG_WARN std::cerr
#define LOG_ERROR std::cerr
namespace std::this_thread { inline void historyTestSleepFor(std::chrono::seconds) {} }
#define sleep_for historyTestSleepFor
static int attempts = 0;
static bool restored = false;
class ChatServer {
public:
    ChatServer(int, const std::string&) {
        if (std::string(std::getenv("HISTORY_STARTUP_SCENARIO")) == "constructor")
            throw std::runtime_error("offline connection-pool initialization failure");
    }
    ~ChatServer() { std::cout << "ATTEMPTS=" << attempts << '\n'; }
    void setThreadNum(int) {}
    void* getLoop() { return nullptr; }
    void initChatMessage() {
        ++attempts;
        const std::string scenario = std::getenv("HISTORY_STARTUP_SCENARIO");
        if (scenario == "permanent" || (scenario == "transient" && attempts < 3))
            throw std::runtime_error("offline query failure");
        restored = true;
    }
    void start() {
        if (!restored) throw std::runtime_error("server began serving before history restoration");
        const std::string scenario = std::getenv("HISTORY_STARTUP_SCENARIO");
        if (scenario == "transient" && attempts != 3) throw std::runtime_error("retry count mismatch");
        std::cout << "SERVING restored history\n";
    }
};
struct GemmaInferencePool {
    static GemmaInferencePool& instance() { static GemmaInferencePool value; return value; }
    void initialize(const std::string&, const std::string&) {}
};
struct MQManager {
    using AckFn = std::function<void(bool)>;
    static MQManager& instance() { static MQManager value; return value; }
    void start(void*, const std::string&, void (*)(const std::string&, uint64_t, AckFn)) {}
    void shutdown() {}
};
static const std::string QUEUE_NAME = "sql_queue";
void executeMysql(const std::string&, uint64_t, MQManager::AckFn) {}
// @STARTUP_METHODS@
