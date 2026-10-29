#include "../include/session/SessionStorage.h"
#include <stdexcept>

namespace http
{

namespace session
{

MemorySessionStorage::MemorySessionStorage(std::size_t capacity) : capacity_(capacity)
{
    if (capacity == 0) throw std::invalid_argument("Session capacity must be positive");
}

void MemorySessionStorage::save(std::shared_ptr<Session> session)
{
    if (!admit(std::move(session))) throw std::length_error("Session admission rejected");
}

bool MemorySessionStorage::admit(std::shared_ptr<Session> session)
{
    if (!session) throw std::invalid_argument("Session must not be null");
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session->getId());
    if (it != sessions_.end())
    {
        if (it->second == session) return true;
        auto previous = it->second;
        std::lock_guard<std::mutex> sessionLock(previous->mutex_);
        if (std::chrono::steady_clock::now() < previous->expiryTime_) return false;
        sessions_.erase(it);
    }
    if (sessions_.size() >= capacity_) cleanExpiredLocked();
    if (sessions_.size() >= capacity_) return false;
    sessions_.emplace(session->getId(), std::move(session));
    return true;
}

// 通过会话ID从存储中加载会话
std::shared_ptr<Session> MemorySessionStorage::load(const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) return nullptr;
    auto session = it->second; // Keep the mutex alive even if erasing the last map owner.
    std::lock_guard<std::mutex> sessionLock(session->mutex_);
    if (std::chrono::steady_clock::now() >= session->expiryTime_)
    {
        sessions_.erase(it);
        return nullptr;
    }
    return session;
}

std::shared_ptr<Session> MemorySessionStorage::loadAndRefresh(const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) return nullptr;
    auto session = it->second;
    std::lock_guard<std::mutex> sessionLock(session->mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (now >= session->expiryTime_)
    {
        sessions_.erase(it);
        return nullptr;
    }
    session->expiryTime_ = now + std::chrono::seconds(session->maxAge_);
    return session;
}

// 通过会话ID从存储中移除会话
void MemorySessionStorage::remove(const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(sessionId);
}

void MemorySessionStorage::cleanExpired()
{
    std::lock_guard<std::mutex> lock(mutex_);
    cleanExpiredLocked();
}

void MemorySessionStorage::cleanExpiredLocked()
{
    for (auto it = sessions_.begin(); it != sessions_.end();)
    {
        auto session = it->second;
        std::lock_guard<std::mutex> sessionLock(session->mutex_);
        if (std::chrono::steady_clock::now() >= session->expiryTime_) it = sessions_.erase(it);
        else ++it;
    }
}

bool MemorySessionStorage::updateExisting(std::shared_ptr<Session> session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session->getId());
    if (it == sessions_.end() || it->second != session) return false;
    // Keep expiry decision and erasure together, including against direct refresh().
    std::lock_guard<std::mutex> sessionLock(session->mutex_);
    if (std::chrono::steady_clock::now() >= session->expiryTime_)
    {
        sessions_.erase(it);
        return false;
    }
    return true; // The memory entry already points at the mutated object.
}

} // namespace session
} // namespace http