#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include "DbConnection.h"
namespace http { namespace db {
// Connections are health-checked only after exclusive removal from the idle queue.
class DbConnectionPool {
public:
    static DbConnectionPool& getInstance() {
        static DbConnectionPool instance;
        return instance;
    }
    void init(const std::string& host, const std::string& user,
              const std::string& password, const std::string& database, size_t poolSize = 10);
    std::shared_ptr<DbConnection> getConnection();
private:
    struct State {
        std::string host, user, password, database;
        std::queue<std::shared_ptr<DbConnection>> idle;
        std::mutex mutex;
        std::condition_variable cv;
        size_t target = 0;
        size_t allocated = 0; // idle + checked out + creation reservations
        bool initialized = false;
        bool stopping = false;
    };
    DbConnectionPool();
    ~DbConnectionPool();
    DbConnectionPool(const DbConnectionPool&) = delete;
    DbConnectionPool& operator=(const DbConnectionPool&) = delete;
    static std::shared_ptr<DbConnection> createConnection(const std::shared_ptr<State>& state);
    std::shared_ptr<State> state_;
};
}} // namespace http::db
