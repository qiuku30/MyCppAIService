#include "../include/handlers/ChatLoginHandler.h"
#include "../../../../HttpServer/include/utils/PasswordUtil.h"
#include <utility>

void ChatLoginHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    
    auto contentType = req.getHeader("Content-Type");
    if (contentType.empty() || contentType != "application/json" || req.getBody().empty())
    {

        resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
        resp->setCloseConnection(true);
        resp->setContentType("application/json");
        resp->setContentLength(0);
        resp->setBody("");
        return;
    }


    try
    {
        json parsed = json::parse(req.getBody());
        std::string username = parsed["username"];
        std::string password = parsed["password"];

        int userId = queryUserId(username, password);
        if (userId != -1)
        {

            auto session = server_->getSessionManager()->getSession(req, resp);


            session->setValue("userId", std::to_string(userId));
            session->setValue("username", username);
            session->setValue("isLoggedIn", "true");
            {
                std::lock_guard<std::mutex> lock(server_->mutexForOnlineUsers_);
                server_->onlineUsers_[userId] = true;
            }

            json successResp;
            successResp["success"] = true;
            successResp["userId"] = userId;
            std::string successBody = successResp.dump(4);

            resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
            resp->setCloseConnection(false);
            resp->setContentType("application/json");
            resp->setContentLength(successBody.size());
            resp->setBody(successBody);
            return;
        }
        else 
        {
            json failureResp;
            failureResp["status"] = "error";
            failureResp["message"] = "Invalid username or password";
            std::string failureBody = failureResp.dump(4);

            resp->setStatusLine(req.getVersion(), http::HttpResponse::k401Unauthorized, "Unauthorized");
            resp->setCloseConnection(false);
            resp->setContentType("application/json");
            resp->setContentLength(failureBody.size());
            resp->setBody(failureBody);
            return;
        }
    }
    catch (const std::exception&)
    {
        json failureResp;
        failureResp["status"] = "error";
        failureResp["message"] = "Login failed";
        std::string failureBody = failureResp.dump(4);

        resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
        resp->setCloseConnection(true);
        resp->setContentType("application/json");
        resp->setContentLength(failureBody.size());
        resp->setBody(failureBody);
        return;
    }

}

int ChatLoginHandler::queryUserId(const std::string& username, const std::string& password)
{
    const auto account = mysqlUtil_.consumeQuery(
        "SELECT id, password FROM users WHERE username = ?",
        [](sql::ResultSet& result) {
            if (!result.next()) return std::make_pair(-1, std::string{});
            return std::make_pair(result.getInt("id"), std::string(result.getString("password")));
        }, username);
    if (account.first == -1) return -1;

    if (http::PasswordUtil::isHash(account.second)) {
        return http::PasswordUtil::verify(password, account.second) ? account.first : -1;
    }
    if (!http::PasswordUtil::verifyLegacy(password, account.second)) return -1;

    // Upgrade only the exact credential just verified; never overwrite a newer password.
    const auto encoded = http::PasswordUtil::hash(password);
    const int changed = mysqlUtil_.executeUpdate(
        "UPDATE users SET password = ? WHERE id = ? AND BINARY password = ?",
        encoded, std::to_string(account.first), account.second);
    return changed == 1 ? account.first : -1;
}
