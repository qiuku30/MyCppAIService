#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>
#include <map>
#include <variant>
namespace fixtureSql {
inline std::atomic<int> results{0}, statements{0}, connections{0};
inline std::vector<std::string> parameters;
inline std::atomic<bool> touchedWithLiveResult{false};
inline std::map<std::string, std::variant<std::string, int, bool>> connectOptions;
inline int postConnectOptions = 0;
}
namespace sql {
using ConnectOptionsMap = std::map<std::string, std::variant<std::string, int, bool>>;
class SQLException : public std::runtime_error { public: using std::runtime_error::runtime_error; };
class ResultSet {
    std::shared_ptr<bool> alive_;
    bool read_ = false;
public:
    explicit ResultSet(std::shared_ptr<bool> alive) : alive_(std::move(alive)) { ++fixtureSql::results; }
    ~ResultSet() { --fixtureSql::results; }
    bool next() {
        if (!*alive_) throw SQLException("statement was destroyed before result consumption");
        bool result = !read_; read_ = true; return result;
    }
};
class Statement {
protected:
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
public:
    Statement() { ++fixtureSql::statements; }
    virtual ~Statement() { *alive_ = false; --fixtureSql::statements; }
    bool execute(const std::string&) { return true; }
    ResultSet* executeQuery(const std::string&) { return new ResultSet(alive_); }
    bool getMoreResults() { return false; }
    ResultSet* getResultSet() { return nullptr; }
};
class PreparedStatement : public Statement {
public:
    void setString(int index, const std::string& value) {
        if (static_cast<int>(fixtureSql::parameters.size()) < index) fixtureSql::parameters.resize(index);
        fixtureSql::parameters[index - 1] = value;
    }
    ResultSet* executeQuery() { return new ResultSet(alive_); }
    int executeUpdate() { return 1; }
};
class Connection {
public:
    Connection() { ++fixtureSql::connections; }
    ~Connection() { --fixtureSql::connections; }
    void setSchema(const std::string&) {}
    void setClientOption(const std::string&, const char*) { ++fixtureSql::postConnectOptions; }
    Statement* createStatement() {
        if (fixtureSql::results) fixtureSql::touchedWithLiveResult = true;
        return new Statement;
    }
    PreparedStatement* prepareStatement(const std::string&) { return new PreparedStatement; }
    bool reconnect() { return true; }
    bool getAutoCommit() { return true; }
    void rollback() {}
    void setAutoCommit(bool) {}
};
namespace mysql {
class MySQL_Driver {
public:
    Connection* connect(const std::string&, const std::string&, const std::string&) { return new Connection; }
    Connection* connect(ConnectOptionsMap& options) { fixtureSql::connectOptions = options; return new Connection; }
};
inline MySQL_Driver* get_mysql_driver_instance() { static MySQL_Driver driver; return &driver; }
}
}
