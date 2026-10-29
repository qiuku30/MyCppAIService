#include "../../../include/utils/db/DbConnection.h"
#include "../../../include/utils/db/DbException.h"
namespace http { namespace db {
DbConnection::DbConnection(const std::string& host, const std::string& user,
                           const std::string& password, const std::string& database)
    : host_(host), user_(user), password_(password), database_(database) { reconnect(); }
DbConnection::~DbConnection() { try { cleanup(); } catch (...) {} }
bool DbConnection::ping() {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        if (!conn_) return false;
        std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
        std::unique_ptr<sql::ResultSet> result(stmt->executeQuery("SELECT 1"));
        return result && result->next();
    } catch (const sql::SQLException&) { return false; }
}
bool DbConnection::isValid() { return ping(); }
void DbConnection::reconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        auto* driver = sql::mysql::get_mysql_driver_instance();
        sql::ConnectOptionsMap options;
        options["hostName"] = host_;
        options["userName"] = user_;
        options["password"] = password_;
        options["schema"] = database_;
        options["OPT_CONNECT_TIMEOUT"] = 10;
        options["OPT_READ_TIMEOUT"] = 10;
        options["OPT_WRITE_TIMEOUT"] = 10;
        options["OPT_RECONNECT"] = false; // The pool exclusively owns reconnect/retry decisions.
        options["CLIENT_MULTI_STATEMENTS"] = false;
        std::shared_ptr<sql::Connection> replacement(driver->connect(options));
        if (!replacement) throw DbException("Database connection failed");
        std::unique_ptr<sql::Statement> stmt(replacement->createStatement());
        stmt->execute("SET NAMES utf8mb4");
        conn_ = std::move(replacement);
    } catch (const sql::SQLException&) { throw DbException("Database connection failed"); }
}
void DbConnection::cleanup() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (conn_ && !conn_->getAutoCommit()) { conn_->rollback(); conn_->setAutoCommit(true); }
}
}} // namespace http::db
