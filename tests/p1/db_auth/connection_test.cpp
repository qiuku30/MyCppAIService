#include "HttpServer/include/utils/db/DbConnection.h"
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
using namespace std::chrono_literals;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << std::endl; std::_Exit(1); }
}
int main() {
    {
        http::db::DbConnection connection("fake", "u", "p", "d");
#ifndef LEGACY_DB_API
        require(fixtureSql::connectOptions.count("OPT_CONNECT_TIMEOUT") && fixtureSql::postConnectOptions == 0,
                "connection options must be applied before the driver connects");
        const auto& options = fixtureSql::connectOptions;
        require(std::get<int>(options.at("OPT_CONNECT_TIMEOUT")) == 10 &&
                std::get<int>(options.at("OPT_READ_TIMEOUT")) == 10 &&
                std::get<int>(options.at("OPT_WRITE_TIMEOUT")) == 10,
                "connect and I/O timeout options use connector integer values");
        require(!std::get<bool>(options.at("OPT_RECONNECT")) &&
                !std::get<bool>(options.at("CLIENT_MULTI_STATEMENTS")),
                "automatic reconnect and multi-statements use false boolean values");
        require(std::get<std::string>(options.at("hostName")) == "fake" &&
                std::get<std::string>(options.at("schema")) == "d",
                "host and selected schema are part of connection creation");
#endif
        std::mutex gateMutex;
        std::condition_variable gateCv;
        bool entered = false, release = false;
        auto consume = [&](sql::ResultSet& result) {
            std::unique_lock<std::mutex> lock(gateMutex);
            entered = true;
            gateCv.notify_all();
            gateCv.wait(lock, [&] { return release; });
            return result.next() ? 42 : -1;
        };
        auto query = std::async(std::launch::async, [&] {
#ifdef LEGACY_DB_API
            std::unique_ptr<sql::ResultSet> result(connection.executeQuery("SELECT id FROM users"));
            return consume(*result);
#else
            return connection.consumeQuery("SELECT id FROM users", consume);
#endif
        });
        {
            std::unique_lock<std::mutex> lock(gateMutex);
            require(gateCv.wait_for(lock, 2s, [&] { return entered; }), "query callback enters");
        }
        auto ping = std::async(std::launch::async, [&] { return connection.ping(); });
        require(ping.wait_for(100ms) == std::future_status::timeout, "ping must wait while a result is being consumed on the same connection");
        auto reconnect = std::async(std::launch::async, [&] { connection.reconnect(); });
        require(reconnect.wait_for(100ms) == std::future_status::timeout, "reconnect must wait while a result is being consumed");
        { std::lock_guard<std::mutex> lock(gateMutex); release = true; }
        gateCv.notify_all();
        require(query.get() == 42, "query materializes result before statement destruction");
        require(ping.get(), "ping completes after callback");
        reconnect.get();
        require(!fixtureSql::touchedWithLiveResult, "driver receives no overlapping operation while a result is live");
#ifndef LEGACY_DB_API
        require(fixtureSql::results == 0 && fixtureSql::statements == 0, "result and statement are reclaimed after callback");
        bool threw = false;
        try {
            connection.consumeQuery("SELECT id FROM users", [](sql::ResultSet&) -> int { throw std::runtime_error("consumer failed"); });
        } catch (const std::runtime_error&) { threw = true; }
        require(threw && fixtureSql::results == 0 && fixtureSql::statements == 0, "consumer exception also releases connector resources");
        std::string mutableValue = "mutable'input";
        connection.executeUpdate("UPDATE users SET password = ? WHERE id = ?", mutableValue, 7);
        require(fixtureSql::parameters == std::vector<std::string>{mutableValue, "7"}, "string lvalues and numeric parameters are both bound");
#endif
    }
    require(fixtureSql::connections == 0, "all connector connections are destroyed");
    std::cout << "PASS: real connection lock, scoped result RAII, callback exception, and parameter binding" << std::endl;
}
