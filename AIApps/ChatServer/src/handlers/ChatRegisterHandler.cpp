#include "../include/handlers/ChatRegisterHandler.h"
#include "../../../../HttpServer/include/utils/PasswordUtil.h"


void ChatRegisterHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    
    json parsed = json::parse(req.getBody());
    std::string username = parsed["username"];
    std::string password = parsed["password"];


    int userId = insertUser(username, password);
    if (userId != -1)
    {

        json successResp;
        successResp["status"] = "success";
        successResp["message"] = "Register successful";
        successResp["userId"] = userId;
        std::string successBody = successResp.dump(4);

        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setCloseConnection(false);
        resp->setContentType("application/json");
        resp->setContentLength(successBody.size());
        resp->setBody(successBody);
    }
    else
    {

        json failureResp;
        failureResp["status"] = "error";
        failureResp["message"] = "username already exists";
        std::string failureBody = failureResp.dump(4);

        resp->setStatusLine(req.getVersion(), http::HttpResponse::k409Conflict, "Conflict");
        resp->setCloseConnection(false);
        resp->setContentType("application/json");
        resp->setContentLength(failureBody.size());
        resp->setBody(failureBody);
    }
}

int ChatRegisterHandler::insertUser(const std::string& username, const std::string& password)
{
    if (isUserExist(username)) return -1;
    const auto encoded = http::PasswordUtil::hash(password);
    mysqlUtil_.executeUpdate("INSERT INTO users (username, password) VALUES (?, ?)", username, encoded);
    return mysqlUtil_.consumeQuery("SELECT id FROM users WHERE username = ?",
        [](sql::ResultSet& result) { return result.next() ? result.getInt("id") : -1; }, username);
}

bool ChatRegisterHandler::isUserExist(const std::string& username)
{
    return mysqlUtil_.consumeQuery("SELECT id FROM users WHERE username = ?",
        [](sql::ResultSet& result) { return result.next(); }, username);
}
