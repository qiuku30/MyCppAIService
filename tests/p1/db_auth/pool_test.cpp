#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <future>
#include <iostream>
#include "HttpServer/include/utils/db/DbConnection.h"
#define private public
#include "HttpServer/include/utils/db/DbConnectionPool.h"
#undef private
using namespace std::chrono_literals;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << std::endl; std::_Exit(1); }
}
int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "exclusive";
    using http::db::DbConnectionPool;
#ifndef LEGACY_POOL_API
    if (mode == "shutdown") {
        auto pool = std::unique_ptr<DbConnectionPool>(new DbConnectionPool);
        pool->init("fake", "u", "p", "d", 1);
        auto state = pool->state_;
        fixture::destructionGate = true;
        auto stopping = std::async(std::launch::async, [&] { pool.reset(); });
        {
            std::unique_lock<std::mutex> lock(fixture::mutex);
            require(fixture::cv.wait_for(lock, 2s, [] { return fixture::destructionEntered; }), "connector destruction entered");
        }
        require(state->mutex.try_lock(), "shutdown must not hold the pool mutex during connector destruction");
        state->mutex.unlock();
        { std::lock_guard<std::mutex> lock(fixture::mutex); fixture::destructionGate = false; }
        fixture::cv.notify_all();
        require(stopping.wait_for(2s) == std::future_status::ready, "shutdown completes after connector cleanup");
        stopping.get();
        std::cout << "PASS: shutdown does no connector destruction under the pool lock" << std::endl;
        return 0;
    }
    if (mode == "checkout_shutdown") {
        auto pool = std::unique_ptr<DbConnectionPool>(new DbConnectionPool);
        pool->init("fake", "u", "p", "d", 1);
        fixture::gate = true;
        auto borrower = std::async(std::launch::async, [&] { return pool->getConnection(); });
        {
            std::unique_lock<std::mutex> lock(fixture::mutex);
            require(fixture::cv.wait_for(lock, 2s, [] { return fixture::entered; }), "checkout captured state before shutdown");
        }
        pool.reset();
        { std::lock_guard<std::mutex> lock(fixture::mutex); fixture::gate = false; }
        fixture::cv.notify_all();
        require(borrower.wait_for(2s) == std::future_status::ready, "in-flight checkout completes without destroyed-owner access");
        auto lease = borrower.get();
        require(lease->ping(), "in-flight checkout retains its connector until lease return");
        lease.reset();
        require(fixture::instances == 0, "concurrent checkout connection released after shutdown");
        std::cout << "PASS: checkout already holding shared state survives owner shutdown" << std::endl;
        return 0;
    }
#endif
    if (mode == "lifetime") {
        auto pool = std::unique_ptr<DbConnectionPool>(new DbConnectionPool);
        pool->init("fake", "u", "p", "d", 1);
        auto lease = pool->getConnection();
        std::weak_ptr<http::db::DbConnection> observer = lease;
        pool.reset();
        require(lease->ping(), "lease remains usable until returned after owner shutdown");
        lease.reset();
        require(fixture::instances == 0, "outstanding lease releases its final connection after shutdown");
        require(observer.expired(), "weak lease observer never extends a connector lifetime");
        std::cout << "PASS: lifetime" << std::endl;
        return 0;
    }
    auto& pool = DbConnectionPool::getInstance();
    if (mode == "exclusive") {
        fixture::gate = true;
        pool.init("fake", "u", "p", "d", 1);
        // The old checker is allowed to enter first; the revised pool has no periodic checker.
        {
            std::unique_lock<std::mutex> lock(fixture::mutex);
            fixture::cv.wait_for(lock, 1500ms, [] { return fixture::entered; });
        }
        auto first = std::async(std::launch::async, [&] { return pool.getConnection(); });
        {
            std::unique_lock<std::mutex> lock(fixture::mutex);
            require(fixture::cv.wait_for(lock, 2s, [] { return fixture::entered; }), "health check entered");
        }
        auto second = std::async(std::launch::async, [&] { return pool.getConnection(); });
        std::this_thread::sleep_for(80ms);
        require(!fixture::overlap, "checker and borrowers must never health-check the same connection concurrently");
        { std::lock_guard<std::mutex> lock(fixture::mutex); fixture::gate = false; }
        fixture::cv.notify_all();
        require(first.wait_for(2s) == std::future_status::ready, "first exclusive lease completes");
        auto lease = first.get();
        require(second.wait_for(50ms) == std::future_status::timeout, "second borrower waits until lease is returned");
        lease.reset();
        require(second.wait_for(2s) == std::future_status::ready, "second borrower proceeds after return");
        second.get().reset();
    } else {
        pool.init("fake", "u", "p", "d", 1);
        auto warm = pool.getConnection(); warm.reset();
        fixture::offline = true;
        bool threw = false;
        try { pool.getConnection(); } catch (const std::exception&) { threw = true; }
        require(threw, "offline checkout reports failure");
        fixture::offline = false;
        auto recovered = std::async(std::launch::async, [&] { return pool.getConnection(); });
        require(recovered.wait_for(2s) == std::future_status::ready, "failed reconnect and create must not permanently lose pool capacity");
        require(recovered.get()->ping(), "new checkout recovers after database comes back");
    }
    std::cout << "PASS: " << mode << std::endl;
}
