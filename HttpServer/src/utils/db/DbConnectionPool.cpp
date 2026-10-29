#include "../../../include/utils/db/DbConnectionPool.h"
#include "../../../include/utils/db/DbException.h"
#include <chrono>
namespace http { namespace db {
DbConnectionPool::DbConnectionPool() : state_(std::make_shared<State>()) {}
DbConnectionPool::~DbConnectionPool() {
    auto state = state_;
    std::queue<std::shared_ptr<DbConnection>> retired;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stopping = true;
        state->allocated -= state->idle.size();
        retired.swap(state->idle);
    }
    state->cv.notify_all();
    // Connector cleanup can perform I/O; destroy retired connections after unlocking.
}
void DbConnectionPool::init(const std::string& host, const std::string& user,
                            const std::string& password, const std::string& database,
                            size_t poolSize) {
    auto state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->initialized) return;
    if (state->stopping || poolSize == 0) throw DbException("Invalid connection pool initialization");
    state->host = host; state->user = user; state->password = password; state->database = database;
    // Publish no partial pool if startup fails.
    std::queue<std::shared_ptr<DbConnection>> ready;
    for (size_t i = 0; i < poolSize; ++i) ready.push(createConnection(state));
    state->idle.swap(ready);
    state->target = state->allocated = poolSize;
    state->initialized = true;
}
std::shared_ptr<DbConnection> DbConnectionPool::createConnection(const std::shared_ptr<State>& state) {
    return std::make_shared<DbConnection>(state->host, state->user, state->password, state->database);
}
std::shared_ptr<DbConnection> DbConnectionPool::getConnection() {
    auto state = state_;
    std::shared_ptr<DbConnection> conn;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        if (!state->initialized || state->stopping) throw DbException("Connection pool is unavailable");
        if (!state->cv.wait_for(lock, std::chrono::seconds(30), [&] {
                return state->stopping || !state->idle.empty() || state->allocated < state->target;
            })) throw DbException("Connection pool exhausted: timeout after 30s");
        if (state->stopping) throw DbException("Connection pool is stopping");
        if (!state->idle.empty()) {
            conn = std::move(state->idle.front()); state->idle.pop();
        } else {
            ++state->allocated; // Reserve capacity before creating outside the pool lock.
        }
    }
    try {
        if (!conn) conn = createConnection(state);
        if (!conn->ping()) {
            try {
                conn->reconnect();
                if (!conn->ping()) throw DbException("Connection is unavailable after reconnect");
            } catch (const std::exception&) {
                conn = createConnection(state);
                if (!conn->ping()) throw DbException("Replacement connection is unavailable");
            }
        }
    } catch (...) {
        { std::lock_guard<std::mutex> lock(state->mutex); --state->allocated; }
        // A later borrower can reserve the lost slot and retry after database recovery.
        state->cv.notify_one();
        throw;
    }
    // Shared state allows outstanding leases to return after the owner has stopped.
    auto* raw = conn.get(); // Capture before moving conn into the deleter.
    return std::shared_ptr<DbConnection>(raw, [state, conn = std::move(conn)](DbConnection*) mutable {
        auto returnState = std::move(state);
        {
            std::lock_guard<std::mutex> lock(returnState->mutex);
            if (returnState->stopping) --returnState->allocated;
            else returnState->idle.push(std::move(conn));
        }
        conn.reset(); // Release outside the pool lock, even while weak observers remain.
        returnState->cv.notify_one();
    });
}
}} // namespace http::db
