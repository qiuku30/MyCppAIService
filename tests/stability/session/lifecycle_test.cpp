#include "session/SessionManager.h"
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <vector>
#include <cstdlib>
#include <iostream>
#include <future>
#include <thread>

using http::session::MemorySessionStorage;
using http::session::Session;
using http::session::SessionManager;
using namespace std::chrono_literals;

namespace {
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
template<class Predicate> bool eventually(Predicate predicate, std::chrono::milliseconds timeout = 3s) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < until);
    return predicate();
}
void automaticCleanup() {
    SessionManager manager(std::make_unique<MemorySessionStorage>(), 20ms, 1);
    http::HttpRequest request;
    http::HttpResponse response;
    auto session = manager.getSession(request, &response);
    std::weak_ptr<Session> weak = session;
    session.reset();
    check(eventually([&] { return weak.expired(); }), "periodic worker reclaims an unvisited expired session");
}
void promptDestruction() {
    auto done = std::async(std::launch::async, [] {
        auto manager = std::make_unique<SessionManager>(std::make_unique<MemorySessionStorage>(), 1h);
        manager.reset();
    });
    check(done.wait_for(1s) == std::future_status::ready, "destructor interrupts an hour-long cleanup wait");
    done.get();
}
// Legacy save/load/remove implementation with an optional throwing cleanup hook.
class ThrowOnceStorage : public http::session::SessionStorage {
public:
    explicit ThrowOnceStorage(std::shared_ptr<std::atomic<int>> calls) : calls_(std::move(calls)) {}
    void save(std::shared_ptr<Session> session) override { memory_.save(std::move(session)); }
    std::shared_ptr<Session> load(const std::string& id) override { return memory_.load(id); }
    void remove(const std::string& id) override { memory_.remove(id); }
    void cleanExpired() override {
        if (calls_->fetch_add(1) == 0) throw std::runtime_error("PRIVATE-CLEANUP-DETAIL-MUST-NOT-BE-LOGGED");
        memory_.cleanExpired();
    }
private:
    std::shared_ptr<std::atomic<int>> calls_;
    MemorySessionStorage memory_;
};
void cleanupException() {
    auto calls = std::make_shared<std::atomic<int>>(0);
    std::weak_ptr<Session> weak;
    {
        SessionManager manager(std::make_unique<ThrowOnceStorage>(calls), 20ms, 1);
        http::HttpRequest request;
        http::HttpResponse response;
        auto session = manager.getSession(request, &response);
        weak = session;
        session.reset();
        check(eventually([&] { return calls->load() >= 2; }), "cleanup worker retries after a storage exception");
        check(eventually([&] { return weak.expired(); }), "retrying cleanup worker still reclaims expired sessions");
    }
    const auto stopped = calls->load();
    std::this_thread::sleep_for(100ms);
    check(calls->load() == stopped, "joined worker cannot invoke cleanup after owner destruction");
}
void policyValidation() {
    auto invalid = [](auto action) {
        bool rejected = false;
        try { action(); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "invalid lifecycle policy rejected explicitly");
    };
    invalid([] { MemorySessionStorage storage(0); });
    invalid([] { SessionManager manager(nullptr); });
    invalid([] { SessionManager manager(std::make_unique<MemorySessionStorage>(), 0ms); });
    invalid([] { SessionManager manager(std::make_unique<MemorySessionStorage>(), -1ms); });
    invalid([] { SessionManager manager(std::make_unique<MemorySessionStorage>(), 1s, 0); });
    invalid([] { SessionManager manager(std::make_unique<MemorySessionStorage>(), 1s, -1); });
}
void cleanup() {
    auto owned = std::make_unique<MemorySessionStorage>();
    auto* storage = owned.get();
    SessionManager manager(std::move(owned));
    auto stale = std::make_shared<Session>("stale", nullptr, 1);
    auto active = std::make_shared<Session>("active", nullptr);
    std::weak_ptr<Session> weak = stale;
    storage->save(stale);
    storage->save(active);
    stale.reset();
    std::this_thread::sleep_for(1100ms);
    check(!weak.expired(), "setup: storage still owns expired entry before manual sweep");
    manager.cleanExpiredSessions();
    check(weak.expired(), "manual sweep reclaims unvisited expired entry");
    check(storage->load("active") == active, "manual sweep preserves valid entry");
}
void staleSave() {
    auto owned = std::make_unique<MemorySessionStorage>();
    auto* storage = owned.get();
    SessionManager manager(std::move(owned));
    http::HttpRequest request;
    http::HttpResponse response;
    auto session = manager.getSession(request, &response);
    manager.destroySession(session->getId());
    session->setValue("after-removal", "local-value");
    check(session->getValue("after-removal") == "local-value", "retained caller can still mutate its object");
    check(!storage->load(session->getId()), "stale auto-save cannot resurrect a removed entry");
    manager.updateSession(session);
    check(!storage->load(session->getId()), "explicit update cannot re-admit removed entry");
}
std::string serialize(const http::HttpResponse& response) {
    muduo::net::Buffer buffer;
    response.appendToBuffer(&buffer);
    return std::string(buffer.peek(), buffer.readableBytes());
}
http::HttpRequest cookieRequest(const std::string& id) {
    http::HttpRequest request;
    const std::string cookie = "Cookie: sessionId=" + id;
    request.addHeader(cookie.data(), cookie.data() + 6, cookie.data() + cookie.size());
    return request;
}
void overloadResponse() {
    auto owned = std::make_unique<MemorySessionStorage>(1);
    auto* storage = owned.get();
    SessionManager manager(std::move(owned));
    http::HttpRequest request;
    http::HttpResponse firstResponse;
    auto active = manager.getSession(request, &firstResponse);
    check(serialize(firstResponse).find("Set-Cookie: sessionId=") != std::string::npos, "successful admission provides session cookie");
    http::HttpResponse existingResponse;
    check(manager.getSession(cookieRequest(active->getId()), &existingResponse) == active, "cookie refresh succeeds at capacity");
    active->setValue("at-cap", "safe");
    check(storage->load(active->getId())->getValue("at-cap") == "safe", "auto-save succeeds at capacity");
    http::HttpResponse failedResponse;
    bool unavailable = false;
    try { manager.getSession(request, &failedResponse); }
    catch (const http::HttpResponse& error) {
        const auto wire = serialize(error);
        unavailable = error.getStatusCode() == 503 && wire.find("503 Service Unavailable") != std::string::npos;
        check(wire.find("Set-Cookie:") == std::string::npos, "thrown overload response has no cookie");
    }
    catch (const std::exception&) { unavailable = false; } // Existing login handlers catch these as 400.
    check(unavailable, "admission rejection bypasses handler std::exception catch with explicit HTTP503");
    check(serialize(failedResponse).find("Set-Cookie:") == std::string::npos, "failed admission never adds usable cookie to caller response");
    check(storage->load(active->getId()) == active, "failed admission preserves active entry");
    manager.destroySession(active->getId());
    http::HttpResponse replacementResponse;
    auto replacement = manager.getSession(request, &replacementResponse);
    check(storage->load(replacement->getId()) == replacement, "failed admission leaves no orphan occupying released capacity");
}
class LegacyStorage : public http::session::SessionStorage {
public:
    void save(std::shared_ptr<Session> session) override { memory_.save(std::move(session)); }
    std::shared_ptr<Session> load(const std::string& id) override { return memory_.load(id); }
    void remove(const std::string& id) override { memory_.remove(id); }
private:
    MemorySessionStorage memory_;
};
void legacyCompatibility() {
    SessionManager manager(std::make_unique<LegacyStorage>());
    http::HttpRequest request;
    http::HttpResponse response;
    auto session = manager.getSession(request, &response);
    session->setValue("legacy", "value");
    http::HttpResponse resumed;
    check(manager.getSession(cookieRequest(session->getId()), &resumed) == session, "legacy storage compiles and resumes sessions through defaults");
    check(session->getValue("legacy") == "value", "legacy update fallback preserves data");
    manager.cleanExpiredSessions();
    manager.destroySession(session->getId());
    session->setValue("removed", "local");
    http::HttpResponse newResponse;
    check(manager.getSession(cookieRequest(session->getId()), &newResponse) != session, "legacy default update does not re-admit removed entry in serial use");
}
// Original storage contract permits load() to return expired stored objects;
// SessionManager historically performed the expiry refusal itself.
class LegacyMapStorage : public http::session::SessionStorage {
public:
    void save(std::shared_ptr<Session> session) override { entries_[session->getId()] = std::move(session); }
    std::shared_ptr<Session> load(const std::string& id) override {
        const auto found = entries_.find(id);
        return found == entries_.end() ? nullptr : found->second;
    }
    void remove(const std::string& id) override { entries_.erase(id); }
private:
    std::unordered_map<std::string, std::shared_ptr<Session>> entries_;
};
void legacyExpiryRefusal() {
    auto owned = std::make_unique<LegacyMapStorage>();
    auto* storage = owned.get();
    SessionManager manager(std::move(owned), 1h);
    auto expired = std::make_shared<Session>("expired-auth", &manager, 1);
    auto active = std::make_shared<Session>("active-auth", &manager);
    expired->setValue("userId", "previous-login");
    active->setValue("userId", "current-login");
    storage->save(expired);
    storage->save(active);
    check(eventually([&] { return expired->isExpired(); }), "setup: legacy map retains an actually expired authenticated session");
    http::HttpResponse replacementResponse;
    auto replacement = manager.getSession(cookieRequest(expired->getId()), &replacementResponse);
    check(replacement != expired && replacement->getId() != expired->getId(), "expired legacy authentication identity is refused");
    check(replacement->getValue("userId").empty(), "expired legacy authentication does not transfer into a new session");
    check(!storage->load(expired->getId()), "expired legacy entry is removed before replacement admission");
    check(expired->isExpired() && expired->getValue("userId") == "previous-login", "expiry refusal preserves retained caller state without renewing it");
    check(serialize(replacementResponse).find("Set-Cookie: sessionId=" + replacement->getId()) != std::string::npos, "newly admitted replacement receives its own cookie");
    http::HttpResponse activeResponse;
    auto resumed = manager.getSession(cookieRequest(active->getId()), &activeResponse);
    check(resumed == active && resumed->getValue("userId") == "current-login", "live legacy identity and authentication remain intact");
    check(serialize(activeResponse).find("Set-Cookie:") == std::string::npos, "live legacy resume does not replace its cookie");
}
void legacyExpiredUpdate() {
    auto owned = std::make_unique<LegacyMapStorage>();
    auto* storage = owned.get();
    SessionManager manager(std::move(owned), 1h);
    auto expired = std::make_shared<Session>("expired-update", &manager, -1);
    expired->setValue("userId", "previous-login");
    storage->save(expired);
    check(!storage->updateExisting(expired), "fallback explicit update refuses an expired existing identity");
    check(!storage->load(expired->getId()), "fallback explicit update removes expired stored identity");
    storage->save(expired); // Explicitly seed it again to exercise the real setValue callback.
    expired->setValue("finished", "local-result");
    check(!storage->load(expired->getId()), "expired legacy auto-save cannot retain an expired stored entry");
    manager.updateSession(expired);
    check(!storage->load(expired->getId()), "later legacy manager update cannot re-admit expired entry");
    check(expired->isExpired() && expired->getValue("userId") == "previous-login" && expired->getValue("finished") == "local-result", "expired retained caller still owns its local data without renewal");
    auto live = std::make_shared<Session>("live-update", nullptr);
    live->setValue("userId", "current-login");
    storage->save(live);
    auto expiredCandidate = std::make_shared<Session>(live->getId(), nullptr, -1);
    expiredCandidate->setValue("userId", "expired-candidate");
    check(!storage->updateExisting(expiredCandidate), "expired detached candidate cannot overwrite a live legacy entry");
    check(storage->load(live->getId()) == live && live->getValue("userId") == "current-login", "refusing expired candidate preserves live identity and authentication");
    storage->save(expired);
    auto renewedCandidate = std::make_shared<Session>(expired->getId(), nullptr);
    check(!storage->updateExisting(renewedCandidate), "live candidate cannot update an already expired stored identity");
    check(!storage->load(expired->getId()), "expired current identity is removed even for a live detached candidate");
}
// load returns a detached object instead of the backend's stored object.
class LegacySnapshotStorage : public http::session::SessionStorage {
public:
    void save(std::shared_ptr<Session> session) override {
        const auto now = std::chrono::steady_clock::now();
        records_[session->getId()] = {now + (session->isExpired() ? -1s : 1s), session->getValue("userId")};
    }
    std::shared_ptr<Session> load(const std::string& id) override {
        const auto found = records_.find(id);
        if (found == records_.end()) return nullptr;
        auto result = std::make_shared<Session>(id, nullptr, std::chrono::steady_clock::now() >= found->second.deadline ? -1 : 1);
        result->setValue("userId", found->second.userId);
        return result;
    }
    void remove(const std::string& id) override { records_.erase(id); }
private:
    struct Record {
        std::chrono::steady_clock::time_point deadline;
        std::string userId;
    };
    std::unordered_map<std::string, Record> records_;
};
void legacyDetachedRefresh() {
    auto owned = std::make_unique<LegacySnapshotStorage>();
    auto* storage = owned.get();
    SessionManager manager(std::move(owned), 1h);
    auto initial = std::make_shared<Session>("snapshot-auth", nullptr, 1);
    initial->setValue("userId", "snapshot-login");
    storage->save(initial);
    std::this_thread::sleep_for(550ms);
    http::HttpResponse response;
    auto resumed = manager.getSession(cookieRequest(initial->getId()), &response);
    check(resumed != initial && resumed->getId() == initial->getId(), "legacy detached load preserves existing identity by ID");
    check(resumed->getValue("userId") == "snapshot-login", "legacy detached refresh preserves authentication data");
    check(eventually([&] { return initial->isExpired(); }), "setup: detached refresh crosses the original backend idle deadline");
    auto persisted = storage->load(initial->getId());
    check(persisted && !persisted->isExpired(), "legacy detached refresh is saved back before backend expiry");
    check(persisted->getValue("userId") == "snapshot-login", "saved detached refresh retains backend authentication data");
}
void legacyDetachedAutoSave() {
    auto owned = std::make_unique<LegacySnapshotStorage>();
    auto* storage = owned.get();
    SessionManager manager(std::move(owned), 1h);
    http::HttpRequest request;
    http::HttpResponse response;
    auto created = manager.getSession(request, &response);
    created->setValue("userId", "created-login");
    check(storage->load(created->getId())->getValue("userId") == "created-login", "legacy detached backend persists new-session auto-save");
    http::HttpResponse resumedResponse;
    auto resumed = manager.getSession(cookieRequest(created->getId()), &resumedResponse);
    check(resumed != created && resumed->getValue("userId") == "created-login", "detached load resumes current authentication by ID");
    resumed->setValue("userId", "resumed-login");
    check(storage->load(created->getId())->getValue("userId") == "resumed-login", "legacy detached backend persists resumed-session auto-save");
}
void boundedAdmission() {
    constexpr int limit = 7;
    MemorySessionStorage storage(limit);
    std::atomic<int> admitted{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    std::vector<std::shared_ptr<Session>> sessions;
    for (int i = 0; i < 48; ++i) sessions.push_back(std::make_shared<Session>(std::to_string(i), nullptr));
    for (const auto& session : sessions) threads.emplace_back([&, session] {
        while (!start.load()) std::this_thread::yield();
        try { storage.save(session); ++admitted; }
        catch (const std::length_error&) {}
    });
    start = true;
    for (auto& thread : threads) thread.join();
    check(admitted == limit, "parallel admissions stop exactly at configured capacity");
    int loaded = 0;
    for (const auto& session : sessions) if (storage.load(session->getId())) {
        ++loaded;
        session->setValue("at-cap", "value");
        storage.save(session);
        check(storage.load(session->getId())->getValue("at-cap") == "value", "existing updates are accepted at capacity");
    }
    check(loaded == limit, "capacity never admits extra map entries");
}
void expiredIdReuse() {
    MemorySessionStorage storage(1);
    auto expired = std::make_shared<Session>("same-id", nullptr, -1);
    std::weak_ptr<Session> weak = expired;
    storage.save(expired);
    expired.reset();
    auto replacement = std::make_shared<Session>("same-id", nullptr);
    check(storage.admit(replacement), "expired matching ID can be explicitly admitted again");
    check(weak.expired() && storage.load("same-id") == replacement, "expired matching ID is reclaimed atomically");
}
void capacityReuse() {
    MemorySessionStorage storage(1);
    auto expired = std::make_shared<Session>("expired", nullptr, -1);
    std::weak_ptr<Session> weak = expired;
    storage.save(expired);
    expired.reset();
    check(!weak.expired(), "setup: expired entry still occupies capacity");
    auto active = std::make_shared<Session>("active", nullptr);
    storage.save(active);
    check(weak.expired(), "admission at capacity reclaims unvisited expired entries");
    check(storage.load("active") == active, "expired capacity is reusable");
    bool rejected = false;
    try { storage.save(std::make_shared<Session>("overflow", nullptr)); }
    catch (const std::length_error&) { rejected = true; }
    check(rejected && storage.load("active") == active, "admission never evicts a valid entry");
    storage.remove("active");
    storage.save(std::make_shared<Session>("replacement", nullptr));
    check(bool(storage.load("replacement")), "explicit removal releases capacity");
}
void concurrentRefresh() {
    auto owned = std::make_unique<MemorySessionStorage>(16);
    auto* storage = owned.get();
    SessionManager manager(std::move(owned), 5ms, 1);
    http::HttpRequest request;
    http::HttpResponse response;
    auto session = manager.getSession(request, &response);
    const auto id = session->getId();
    const auto cookie = cookieRequest(id);
    std::weak_ptr<Session> weak = session;
    std::atomic<bool> stop{false};
    std::atomic<int> refreshed{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 3; ++i) threads.emplace_back([&] {
        while (!stop.load()) {
            http::HttpResponse resumed;
            auto loaded = manager.getSession(cookie, &resumed);
            check(loaded == session, "load/refresh remains the same live object during concurrent sweeps");
            loaded->setValue("active", "value");
            ++refreshed;
            std::this_thread::sleep_for(1ms);
        }
    });
    threads.emplace_back([&] {
        while (!stop.load()) {
            manager.cleanExpiredSessions();
            http::HttpResponse extraResponse;
            auto extra = manager.getSession(request, &extraResponse);
            manager.destroySession(extra->getId());
            std::this_thread::sleep_for(1ms);
        }
    });
    const auto started = std::chrono::steady_clock::now();
    check(eventually([&] { return refreshed.load() > 100 && std::chrono::steady_clock::now() - started > 1400ms; }), "refreshes span the original idle deadline");
    stop = true;
    for (auto& thread : threads) thread.join();
    check(storage->load(id) == session && !session->isExpired(), "active entry survives concurrent expiry checks");
    session.reset();
    check(eventually([&] { return weak.expired(); }), "active entry becomes reclaimable after refresh stops");
}
void concurrentManagerAdmission() {
    constexpr int limit = 7;
    auto owned = std::make_unique<MemorySessionStorage>(limit);
    auto* storage = owned.get();
    SessionManager manager(std::move(owned), 1h);
    std::vector<std::shared_ptr<Session>> accepted(48);
    std::atomic<int> admitted{0}, rejected{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < accepted.size(); ++i) threads.emplace_back([&, i] {
        http::HttpRequest request;
        http::HttpResponse response;
        while (!go.load()) std::this_thread::yield();
        try { accepted[i] = manager.getSession(request, &response); ++admitted; }
        catch (const http::HttpResponse& error) {
            check(error.getStatusCode() == 503, "concurrent rejection reports capacity status");
            check(serialize(response).find("Set-Cookie:") == std::string::npos, "rejected creation never gives cookie");
            ++rejected;
        }
    });
    go = true;
    for (auto& thread : threads) thread.join();
    check(admitted == limit && admitted + rejected == int(accepted.size()), "manager concurrent creation obeys configured cap");
    for (const auto& session : accepted) if (session) {
        session->setValue("at-cap", "valid");
        check(storage->load(session->getId()) == session, "admitted session remains accessible at cap");
    }
}
void staleExpiryAndIdentity() {
    auto owned = std::make_unique<MemorySessionStorage>();
    auto* storage = owned.get();
    SessionManager manager(std::move(owned), 1h, 1);
    http::HttpRequest request;
    http::HttpResponse response;
    auto stale = manager.getSession(request, &response);
    const auto id = stale->getId();
    std::this_thread::sleep_for(1100ms);
    manager.cleanExpiredSessions();
    stale->refresh();
    stale->setValue("finished", "late");
    check(!storage->load(id), "expired caller cannot resurrect its entry even after local refresh");
    auto replacement = std::make_shared<Session>(id, nullptr);
    storage->save(replacement);
    stale->setValue("finished", "later");
    manager.updateSession(stale);
    check(storage->load(id) == replacement, "stale callback cannot overwrite a newer entry with the same ID");
    check(replacement->getValue("finished").empty(), "replacement data is preserved");
}
void defaultCapacity() {
    MemorySessionStorage storage;
    for (int i = 0; i < 10000; ++i) check(storage.admit(std::make_shared<Session>(std::to_string(i), nullptr)), "default capacity permits 10000 sessions");
    check(!storage.admit(std::make_shared<Session>("overflow", nullptr)), "default capacity is bounded at 10000 sessions");
    check(!storage.load("0")->isExpired(), "default idle lifetime remains positive and live");
}
void retainedCaller() {
    auto manager = std::make_unique<SessionManager>(std::make_unique<MemorySessionStorage>());
    http::HttpRequest request;
    http::HttpResponse response;
    auto session = manager->getSession(request, &response);
    manager.reset();
    session->setValue("after-destruction", "safe");
    check(session->getValue("after-destruction") == "safe", "retained caller survives manager destruction");
    check(session->getManager() == nullptr, "destroyed manager pointer is not exposed");
    auto replacement = std::make_unique<SessionManager>(std::make_unique<MemorySessionStorage>());
    session->setManager(replacement.get());
    check(session->getManager() == replacement.get(), "callback rebinding follows replacement manager");
    replacement.reset();
    session->setValue("after-replacement", "safe");
    check(session->getManager() == nullptr, "replacement destruction expires manager binding");
}
}
int main(int argc, char** argv) {
    const std::string selected = argc > 1 ? argv[1] : "all";
    unsigned count = 0;
    auto run = [&](const char* name, auto test) {
        if (selected == "all" || selected == name) { test(); ++count; std::cout << "PASS: " << name << '\n'; }
    };
    run("cleanup", cleanup);
    run("stale", staleSave);
    run("retained", retainedCaller);
    run("capacity", boundedAdmission);
    run("reuse", capacityReuse);
    run("overload", overloadResponse);
    run("automatic", automaticCleanup);
    run("shutdown", promptDestruction);
    run("exception", cleanupException);
    run("policy", policyValidation);
    run("refresh", concurrentRefresh);
    run("manager-capacity", concurrentManagerAdmission);
    run("stale-expiry", staleExpiryAndIdentity);
    run("defaults", defaultCapacity);
    run("expired-id", expiredIdReuse);
    run("legacy", legacyCompatibility);
    run("legacy-expiry", legacyExpiryRefusal);
    run("legacy-expired-update", legacyExpiredUpdate);
    run("legacy-detached-refresh", legacyDetachedRefresh);
    run("legacy-detached-update", legacyDetachedAutoSave);
    check(count != 0, "known test selected");
    std::cout << "PASS: " << count << " Session lifecycle cases\n";
}
