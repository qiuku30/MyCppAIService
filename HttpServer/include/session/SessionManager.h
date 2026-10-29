#pragma once

#include "SessionStorage.h"
#include "../http/HttpRequest.h"
#include "../http/HttpResponse.h"
#include <memory>
#include <mutex>
#include <random>
#include <chrono>
#include <condition_variable>
#include <thread>

namespace http
{
namespace session
{

class SessionManager
{
public:
    explicit SessionManager(std::unique_ptr<SessionStorage> storage,
                            std::chrono::milliseconds cleanupInterval = std::chrono::seconds(60),
                            int sessionMaxAge = 3600);
    ~SessionManager();
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    // 从请求中获取或创建会话
    std::shared_ptr<Session> getSession(const HttpRequest& req, HttpResponse* resp);
    
     // 销毁会话
    void destroySession(const std::string& sessionId);

    // 清理过期会话
    void cleanExpiredSessions();

    // 更新会话
    void updateSession(std::shared_ptr<Session> session)
    {
        storage_->updateExisting(std::move(session));
    }
private:
    void cleanupLoop();
    std::string generateSessionId();
    std::string getSessionIdFromCookie(const HttpRequest& req);
    void setSessionCookie(const std::string& sessionId, HttpResponse* resp);

private:
    friend class Session;
    std::shared_ptr<SessionStorage> storage_;
    std::shared_ptr<void> lifetime_ = std::make_shared<int>(0);
    std::mutex rngMutex_;
    std::mt19937 rng_; // 用于生成随机会话id
    const std::chrono::milliseconds cleanupInterval_;
    const int sessionMaxAge_;
    std::mutex cleanupMutex_;
    std::condition_variable cleanupCv_;
    bool stopping_ = false;
    std::thread cleanupThread_;
};

} // namespace session
} // namespace http