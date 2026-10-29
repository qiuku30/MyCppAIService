#include"../include/session/SessionManager.h"
#include <iomanip>
#include <cstdio>
#include <sstream>
#include <stdexcept>

namespace http
{
namespace session
{

// 初始化会话管理器，设置会话存储对象和随机数生成器
SessionManager::SessionManager(std::unique_ptr<SessionStorage> storage,
                               std::chrono::milliseconds cleanupInterval,
                               int sessionMaxAge)
    : storage_(std::move(storage))
    , rng_(std::random_device{}())
    , cleanupInterval_(cleanupInterval)
    , sessionMaxAge_(sessionMaxAge)
{
    if (!storage_) throw std::invalid_argument("Session storage must not be null");
    if (cleanupInterval_.count() <= 0) throw std::invalid_argument("Session cleanup interval must be positive");
    if (sessionMaxAge_ <= 0) throw std::invalid_argument("Session lifetime must be positive");
    cleanupThread_ = std::thread(&SessionManager::cleanupLoop, this);
}

SessionManager::~SessionManager()
{
    {
        std::lock_guard<std::mutex> lock(cleanupMutex_);
        stopping_ = true;
    }
    cleanupCv_.notify_all();
    if (cleanupThread_.joinable()) cleanupThread_.join();
    lifetime_.reset();
}

void SessionManager::cleanupLoop()
{
    std::unique_lock<std::mutex> lock(cleanupMutex_);
    while (!cleanupCv_.wait_for(lock, cleanupInterval_, [this] { return stopping_; }))
    {
        lock.unlock();
        try
        {
            cleanExpiredSessions();
        }
        catch (...)
        {
            // Fixed diagnostic only: backend exception payloads may contain secrets.
            std::fputs("session_cleanup_error category=storage_exception\n", stderr);
        }
        lock.lock();
    }
}

// 从请求中获取或创建会话，也就是说，如果请求中包含会话ID，则从存储中加载会话，否则创建一个新的会话
std::shared_ptr<Session> SessionManager::getSession(const HttpRequest& req, HttpResponse* resp)
{
    const auto sessionId = getSessionIdFromCookie(req);
    auto session = sessionId.empty() ? nullptr : storage_->loadAndRefresh(sessionId);
    if (session)
    {
        session->setManager(this);
        return session;
    }

    session = std::make_shared<Session>(generateSessionId(), this, sessionMaxAge_);
    if (!storage_->admit(session))
    {
        // HttpServer already propagates HttpResponse exceptions. In particular,
        // this bypasses login handlers' std::exception -> HTTP400 catch blocks.
        HttpResponse unavailable(true);
        unavailable.setStatusCode(HttpResponse::k503ServiceUnavailable);
        unavailable.setContentType("text/plain; charset=utf-8");
        unavailable.setBody("Session capacity unavailable");
        throw unavailable;
    }
    setSessionCookie(session->getId(), resp);
    return session;
}

// 生成唯一的会话标识符，确保会话的唯一性和安全性
std::string SessionManager::generateSessionId()
{
    std::lock_guard<std::mutex> lock(rngMutex_);
    std::stringstream ss;
    std::uniform_int_distribution<> dist(0, 15);

    // 生成32个字符的会话ID，每个字符是一个十六进制数字
    for (int i = 0; i < 32; ++i)
    {
        ss << std::hex << dist(rng_);
    }
    return ss.str();
}

void SessionManager::destroySession(const std::string& sessionId)
{
    storage_->remove(sessionId);
}

void SessionManager::cleanExpiredSessions()
{
    storage_->cleanExpired();
}

std::string SessionManager::getSessionIdFromCookie(const HttpRequest& req)
{
    std::string sessionId;
    std::string cookie = req.getHeader("Cookie");

    if (!cookie.empty())
    {
        size_t pos = cookie.find("sessionId=");
        if (pos != std::string::npos)
        {
            pos += 10; // 跳过"sessionId="
            size_t end = cookie.find(';', pos);
            if (end != std::string::npos)
            {
                sessionId = cookie.substr(pos, end - pos);
            }
            else
            {
                sessionId = cookie.substr(pos);
            }
        }
    }
    
    return sessionId;
}

void SessionManager::setSessionCookie(const std::string& sessionId, HttpResponse* resp)
{
    // 设置会话ID到响应头中，作为Cookie
    std::string cookie = "sessionId=" + sessionId + "; Path=/; HttpOnly";
    resp->addHeader("Set-Cookie", cookie);
}

} // namespace session
} // namespace http