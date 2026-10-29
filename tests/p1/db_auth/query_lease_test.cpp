#include "HttpServer/include/utils/MysqlUtil.h"
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
using namespace std::chrono_literals;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << std::endl; std::_Exit(1); }
}
int main() {
    http::MysqlUtil::init("fake", "u", "p", "d", 1);
    http::MysqlUtil util;
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, release = false;
    auto query = std::async(std::launch::async, [&] {
        try {
            util.consumeQuery("SELECT id FROM users", [&](sql::ResultSet& result) -> int {
                std::unique_lock<std::mutex> lock(mutex);
                entered = true;
                cv.notify_all();
                cv.wait(lock, [&] { return release; });
                require(result.next(), "result remains readable while callback owns the pool lease");
                throw std::runtime_error("callback abort");
            });
        } catch (const std::runtime_error&) { return true; }
        return false;
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(cv.wait_for(lock, 2s, [&] { return entered; }), "pool callback entered");
    }
    auto update = std::async(std::launch::async, [&] { return util.executeUpdate("UPDATE users SET password = ?", std::string("encoded")); });
    require(update.wait_for(100ms) == std::future_status::timeout, "another borrower waits until result callback ends");
    { std::lock_guard<std::mutex> lock(mutex); release = true; }
    cv.notify_all();
    require(query.get(), "callback failure propagates");
    require(update.wait_for(2s) == std::future_status::ready && update.get() == 1, "callback failure returns its pool lease");
    require(fixtureSql::results == 0 && fixtureSql::statements == 0, "no connector resources escape MysqlUtil");
    require(!fixtureSql::touchedWithLiveResult, "pool cannot reuse connection during result consumption");
    std::cout << "PASS: MysqlUtil owns lease through callback and returns it on exception" << std::endl;
}
