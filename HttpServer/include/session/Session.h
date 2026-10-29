#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <chrono>

namespace http
{

namespace session
{

class SessionManager;
class SessionStorage;
class MemorySessionStorage;

class Session : public std::enable_shared_from_this<Session>
{
public:
    Session(const std::string& sessionId, SessionManager* sessionManager, int maxAge = 3600); // 默认1小时过期
    
    const std::string& getId() const 
    { return sessionId_; }

    bool isExpired() const;
    void refresh(); // 刷新过期时间

    // Rebinding is synchronized with setValue's callback snapshot.
    void setManager(SessionManager* sessionManager);
    SessionManager* getManager() const;

    // 数据存取
    void setValue(const std::string&key, const std::string&value);
    std::string getValue(const std::string&key) const;
    void remove(const std::string&key);
    void clear();
private:
    friend class MemorySessionStorage; // Storage holds its map lock before the Session lock.
    mutable std::mutex mutex_; // 保护 data_、expiryTime_ 和 sessionManager_
    std::string                                  sessionId_;
    std::unordered_map<std::string, std::string> data_;
    std::chrono::steady_clock::time_point        expiryTime_;
    int                                          maxAge_; // 过期时间（秒）
    SessionManager*                              sessionManager_;
    std::weak_ptr<SessionStorage>                 updateStorage_;
    std::weak_ptr<void>                           managerLifetime_;
};

} // namespace session
} // namespace http