#pragma once

#include <mutex>
#include "Session.h"
#include <memory>
#include <cstddef>
#include <utility>

namespace http
{
namespace session
{

class SessionStorage
{
public:
    virtual ~SessionStorage() = default;
    virtual void save(std::shared_ptr<Session> session) = 0;
    virtual std::shared_ptr<Session> load(const std::string& sessionId) = 0;
    virtual void remove(const std::string& sessionId) = 0;
    // Defaults preserve existing subclasses. Concurrent backends must override
    // lifecycle operations for atomic refresh/update, bounded admission and sweeps.
    // MemorySessionStorage below implements those guarantees.
    virtual void cleanExpired() {}
    // Explicit admission is separate from auto-saving a retained object.
    virtual bool admit(std::shared_ptr<Session> session)
    {
        save(std::move(session));
        return true;
    }
    virtual std::shared_ptr<Session> loadAndRefresh(const std::string& sessionId)
    {
        auto session = load(sessionId);
        if (!session) return nullptr;
        // Legacy load() may return expired entries. Refuse authentication before
        // refreshing, and persist refreshes for backends returning snapshots.
        if (session->isExpired())
        {
            remove(sessionId);
            return nullptr;
        }
        session->refresh();
        save(session);
        return session;
    }
    // Serial compatibility uses ID presence, since legacy loads may be detached
    // snapshots. Atomic replacement/generation checks require a backend override.
    virtual bool updateExisting(std::shared_ptr<Session> session)
    {
        auto current = load(session->getId());
        if (!current) return false;
        if (current->isExpired())
        {
            remove(session->getId());
            return false;
        }
        if (session->isExpired()) return false;
        save(std::move(session));
        return true;
    }
};

// 基于内存的会话存储实现
class MemorySessionStorage : public SessionStorage
{
public:
    explicit MemorySessionStorage(std::size_t capacity = 10000);
    void save(std::shared_ptr<Session> session) override;
    std::shared_ptr<Session> load(const std::string& sessionId) override;
    void remove(const std::string& sessionId) override;
    void cleanExpired() override;
    bool updateExisting(std::shared_ptr<Session> session) override;
    bool admit(std::shared_ptr<Session> session) override;
    std::shared_ptr<Session> loadAndRefresh(const std::string& sessionId) override;
private:
    void cleanExpiredLocked();
    const std::size_t capacity_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
};

} // namespace session
} // namespace http