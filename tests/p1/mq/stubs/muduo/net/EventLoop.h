#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace muduo { class Timestamp {}; namespace net {
struct TimerId { uint64_t value = 0; };
class EventLoop {
public:
    struct Timer { TimerId id; double delay; std::function<void()> callback; };
    std::deque<std::function<void()>> queued;
    std::deque<Timer> timers;
    std::mutex queueMutex;
    uint64_t nextTimer = 1;
    bool isInLoopThread() const { return true; }
    void assertInLoopThread() const {}
    void queueInLoop(std::function<void()> callback) {
        std::lock_guard<std::mutex> lock(queueMutex);
        queued.push_back(std::move(callback));
    }
    void runInLoop(std::function<void()> callback) { callback(); }
    TimerId runAfter(double delay, std::function<void()> callback) {
        const TimerId id{nextTimer++};
        timers.push_back({id, delay, std::move(callback)});
        return id;
    }
    void cancel(TimerId id) {
        timers.erase(std::remove_if(timers.begin(), timers.end(),
            [id](const Timer& timer) { return timer.id.value == id.value; }), timers.end());
    }
    void flushBatch() {
        std::deque<std::function<void()>> work;
        { std::lock_guard<std::mutex> lock(queueMutex); work.swap(queued); }
        for (auto& callback : work) callback();
    }
    void flush() {
        for (int batch = 0; batch < 1000; ++batch) {
            { std::lock_guard<std::mutex> lock(queueMutex); if (queued.empty()) return; }
            flushBatch();
        }
        throw std::runtime_error("event-loop callback queue did not quiesce");
    }
    void tick() {
        auto work = std::move(timers);
        timers.clear();
        for (auto& timer : work) timer.callback();
        flush();
    }
};
}}
