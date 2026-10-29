#pragma once
#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <iostream>
#include <utility>
#include <memory>
namespace sql {
class ResultSet {
    bool read_ = false;
public:
    bool present = false;
    int id = 7;
    std::string password;
    bool next() { return present && !std::exchange(read_, true); }
    int getInt(const std::string&) { return id; }
    std::string getString(const std::string&) { return password; }
};
}
struct Query { std::string text; std::vector<std::string> parameters; };
struct Recorder {
    std::vector<Query> calls;
    std::string storedPassword;
    bool exists = false;
    bool failUpdate = false;
    int updatedRows = 1;
    sql::ResultSet result;
    template<class... A> sql::ResultSet* executeQuery(const std::string& sql, A&&... args) {
        calls.push_back({sql, {std::forward<A>(args)...}});
        result = {};
        result.present = exists;
        result.password = storedPassword;
        return &result;
    }
    template<class F, class... A> auto consumeQuery(const std::string& sql, F&& fn, A&&... args) {
        auto result = executeQuery(sql, std::forward<A>(args)...);
        return fn(*result);
    }
    template<class... A> int executeUpdate(const std::string& sql, A&&... args) {
        calls.push_back({sql, {std::forward<A>(args)...}});
        if (sql.find("INSERT") == 0) {
            exists = true;
            if (calls.back().parameters.size() >= 2) storedPassword = calls.back().parameters[1];
            else storedPassword = "old plaintext SQL";
        } else if (sql.find("UPDATE") == 0) {
            if (failUpdate) throw std::runtime_error("database update failed");
            if (updatedRows == 1 && !calls.back().parameters.empty()) storedPassword = calls.back().parameters[0];
            return updatedRows;
        }
        return 1;
    }
};
class ChatRegisterHandler {
public:
    Recorder mysqlUtil_;
    int insertUser(const std::string&, const std::string&);
    bool isUserExist(const std::string&);
};
class ChatLoginHandler {
public:
    Recorder mysqlUtil_;
    int queryUserId(const std::string&, const std::string&);
};
