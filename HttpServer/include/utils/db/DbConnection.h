#pragma once
#include <memory>
#include <string>
#include <mutex>
#include <type_traits>
#include <utility>
#include <cppconn/connection.h>
#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>
#include <mysql_driver.h>
#include <mysql/mysql.h>
#include <muduo/base/Logging.h>
#include "DbException.h"
namespace http { namespace db {
class DbConnection {
public:
    DbConnection(const std::string& host, const std::string& user,
                 const std::string& password, const std::string& database);
    ~DbConnection();
    DbConnection(const DbConnection&) = delete;
    DbConnection& operator=(const DbConnection&) = delete;
    bool isValid();
    bool ping();
    void reconnect();
    void cleanup();
    // The callback consumes results while the statement, mutex, and pool lease live.
    template<typename Consumer, typename... Args>
    auto consumeQuery(const std::string& query, Consumer&& consume, Args&&... args) {
        using Value = std::invoke_result_t<Consumer, sql::ResultSet&>;
        static_assert(!std::is_reference_v<Value> && !std::is_pointer_v<Value>,
                      "Query consumers must return materialized values");
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            std::unique_ptr<sql::PreparedStatement> stmt(conn_->prepareStatement(query));
            bindParams(stmt.get(), 1, std::forward<Args>(args)...);
            std::unique_ptr<sql::ResultSet> result(stmt->executeQuery());
            return std::forward<Consumer>(consume)(*result);
        } catch (const sql::SQLException&) {
            LOG_ERROR << "Database query failed";
            throw DbException("Database query failed");
        }
    }
    template<typename... Args>
    int executeUpdate(const std::string& query, Args&&... args) {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            std::unique_ptr<sql::PreparedStatement> stmt(conn_->prepareStatement(query));
            bindParams(stmt.get(), 1, std::forward<Args>(args)...);
            return stmt->executeUpdate();
        } catch (const sql::SQLException&) {
            LOG_ERROR << "Database update failed";
            throw DbException("Database update failed");
        }
    }
private:
    void bindParams(sql::PreparedStatement*, int) {}
    template<typename T, typename... Args>
    void bindParams(sql::PreparedStatement* stmt, int index, T&& value, Args&&... args) {
        if constexpr (std::is_convertible_v<T, std::string>) stmt->setString(index, std::forward<T>(value));
        else stmt->setString(index, std::to_string(std::forward<T>(value)));
        bindParams(stmt, index + 1, std::forward<Args>(args)...);
    }
    std::shared_ptr<sql::Connection> conn_;
    std::string host_, user_, password_, database_;
    std::mutex mutex_;
};
}} // namespace http::db
