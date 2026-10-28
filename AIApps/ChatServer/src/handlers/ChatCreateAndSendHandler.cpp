#include "../include/handlers/ChatCreateAndSendHandler.h"
#include <muduo/net/Buffer.h>
#include <thread>


void ChatCreateAndSendHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    try
    {

        auto session = server_->getSessionManager()->getSession(req, resp);
        LOG_INFO << "session->getValue(\"isLoggedIn\") = " << session->getValue("isLoggedIn");
        if (session->getValue("isLoggedIn") != "true")
        {

            json errorResp;
            errorResp["status"] = "error";
            errorResp["message"] = "Unauthorized";
            std::string errorBody = errorResp.dump(4);

            server_->packageResp(req.getVersion(), http::HttpResponse::k401Unauthorized,
                "Unauthorized", true, "application/json", errorBody.size(),
                errorBody, resp);
            return;
        }


        int userId = std::stoi(session->getValue("userId"));
        std::string username = session->getValue("username");

        std::string userQuestion;
        std::string modelType;
        bool stream = false;

        auto body = req.getBody();
        if (!body.empty()) {
            auto j = json::parse(body);
            if (j.contains("question")) userQuestion = j["question"];
            modelType = j.contains("modelType") ? j["modelType"].get<std::string>() : "1";
            stream = j.value("stream", false);
        }

        AISessionIdGenerator generator;
        std::string sessionId = generator.generate();
        std::cout<<"ɵsessionIdΪ "<<sessionId<<std::endl;


        std::shared_ptr<AIHelper> AIHelperPtr;
        {
            std::lock_guard<std::mutex> lock(server_->mutexForChatInformation);

            auto& userSessions = server_->chatInformation[userId];

            if (userSessions.find(sessionId) == userSessions.end()) {

                userSessions.emplace(
                    sessionId,
                    std::make_shared<AIHelper>()
                );
                server_->sessionsIdsMap[userId].push_back(sessionId);
            }
            AIHelperPtr= userSessions[sessionId];

        }

        // === SSE 流式路径 ===
        if (stream) {
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
            resp->setupSSEHeaders();

            auto conn = req.getConnection();
            {
                muduo::net::Buffer headerBuf;
                resp->appendToBuffer(&headerBuf);
                conn->send(&headerBuf);
            }

            // 先发送 sessionId 事件
            conn->send("data: " + json{{"sessionId", sessionId}}.dump() + "\n\n");

            std::thread([AIHelperPtr, userId, username = std::move(username),
                         sessionId = std::move(sessionId),
                         userQuestion = std::move(userQuestion),
                         modelType = std::move(modelType), conn]() {
                AIHelperPtr->chatStream(userId, username, sessionId,
                    userQuestion, modelType,
                    [conn](const std::string& token) {
                        json event = {
                            {"choices", {{{"delta", {{"content", token}}}, {"index", 0}}}}
                        };
                        conn->send("data: " + event.dump() + "\n\n");
                    },
                    [conn](const std::string& /*full_answer*/) {
                        conn->send("data: [DONE]\n\n");
                        conn->shutdown();
                    },
                    [conn](const std::string& err) {
                        json errEvent = {{"error", err}};
                        conn->send("data: " + errEvent.dump() + "\n\n");
                        conn->send("data: [DONE]\n\n");
                        conn->shutdown();
                    });
            }).detach();

            return;
        }

        // === 非流式路径（原有行为） ===
        std::string aiInformation=AIHelperPtr->chat(userId, username,sessionId, userQuestion, modelType);
        json successResp;
        successResp["success"] = true;
        successResp["Information"] = aiInformation;
        successResp["sessionId"] = sessionId;

        std::string successBody = successResp.dump(4);

        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setCloseConnection(false);
        resp->setContentType("application/json");
        resp->setContentLength(successBody.size());
        resp->setBody(successBody);
        return;
    }
    catch (const std::exception& e)
    {

        json failureResp;
        failureResp["status"] = "error";
        failureResp["message"] = e.what();
        std::string failureBody = failureResp.dump(4);
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
        resp->setCloseConnection(true);
        resp->setContentType("application/json");
        resp->setContentLength(failureBody.size());
        resp->setBody(failureBody);
    }
}









