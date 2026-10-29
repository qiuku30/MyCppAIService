#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>
namespace fixture {
inline std::mutex mutex;
inline std::condition_variable cv;
inline bool gate = false;
inline bool entered = false;
inline bool destructionGate = false;
inline bool destructionEntered = false;
inline std::atomic<bool> offline{false};
inline std::atomic<int> active{0};
inline std::atomic<bool> overlap{false};
inline std::atomic<int> instances{0};
}
namespace http { namespace db {
class DbConnection {
public:
    DbConnection(const std::string&, const std::string&, const std::string&, const std::string&) {
        if (fixture::offline) throw std::runtime_error("offline create");
        ++fixture::instances;
    }
    ~DbConnection() {
        std::unique_lock<std::mutex> lock(fixture::mutex);
        fixture::destructionEntered = true;
        fixture::cv.notify_all();
        fixture::cv.wait(lock, [] { return !fixture::destructionGate; });
        --fixture::instances;
    }
    bool ping() {
        if (fixture::active.fetch_add(1)) fixture::overlap = true;
        {
            std::unique_lock<std::mutex> lock(fixture::mutex);
            fixture::entered = true;
            fixture::cv.notify_all();
            fixture::cv.wait(lock, [] { return !fixture::gate; });
        }
        --fixture::active;
        return !fixture::offline;
    }
    void reconnect() { if (fixture::offline) throw std::runtime_error("offline reconnect"); }
};
}}
