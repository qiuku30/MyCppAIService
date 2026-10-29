#include "../include/session/Session.h"

#include "../include/session/SessionManager.h"

namespace http
{
namespace session
{

Session::Session(const std::string& sessionId, SessionManager* sessionManager, int maxAge)
    : sessionId_(sessionId)
    , maxAge_(maxAge)
    , sessionManager_(sessionManager)
{
    setManager(sessionManager);
    refresh(); // 初始化时设置过期时间
}

// 检查会话是否已过期
bool Session::isExpired() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return std::chrono::steady_clock::now() >= expiryTime_;
}

// 刷新会话的过期时间
void Session::refresh()
{
    std::lock_guard<std::mutex> lock(mutex_);
    expiryTime_ = std::chrono::steady_clock::now() + std::chrono::seconds(maxAge_);
}

// 设置会话数据
void Session::setValue(const std::string& key, const std::string& value)
{
    std::weak_ptr<SessionStorage> target;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        data_[key] = value;
        target = updateStorage_;
    }
    // Never call through the informational raw manager pointer. The callback
    // retains storage only while updating, after releasing the Session lock.
    if (auto storage = target.lock()) storage->updateExisting(shared_from_this());
}

void Session::setManager(SessionManager* manager)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sessionManager_ = manager;
    updateStorage_ = manager ? manager->storage_ : std::weak_ptr<SessionStorage>{};
    managerLifetime_ = manager ? manager->lifetime_ : std::weak_ptr<void>{};
}

SessionManager* Session::getManager() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return managerLifetime_.expired() ? nullptr : sessionManager_;
}

// 获取会话数据
std::string Session::getValue(const std::string& key) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = data_.find(key);
    return it != data_.end() ? it->second : std::string();
}

// 删除会话数据
void Session::remove(const std::string& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    data_.erase(key);
}

// 清空会话数据
void Session::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    data_.clear();
}

} // namespace session
} // namespace http