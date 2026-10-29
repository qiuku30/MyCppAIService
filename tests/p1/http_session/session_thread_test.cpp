#include "session/SessionManager.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <set>
#include <thread>
#include <vector>

using http::session::MemorySessionStorage;
using http::session::Session;
using http::session::SessionManager;

namespace {
constexpr int workers = 4;
constexpr int iterations = 400;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::abort(); }
}
template<class Task> void parallel(Task task) {
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&, worker] {
            ready.fetch_add(1);
            while (!go.load()) std::this_thread::yield();
            task(worker);
        });
    }
    while (ready.load() != workers) std::this_thread::yield();
    go.store(true);
    for (auto& thread : threads) thread.join();
}

void storageThreads() {
    MemorySessionStorage storage;
    parallel([&](int worker) {
        for (int i = 0; i < iterations; ++i) {
            const auto id = std::to_string(worker) + "/" + std::to_string(i);
            auto session = std::make_shared<Session>(id, nullptr);
            storage.save(session);
            check(storage.load(id) == session, "saved session is available during concurrent map writes");
            storage.remove(id);
            check(!storage.load(id), "removed session is unavailable");
            auto expired = std::make_shared<Session>(id, nullptr, -1);
            storage.save(expired);
            check(!storage.load(id), "expired session is evicted during concurrent map changes");
        }
    });
}

void dataThreads() {
    auto session = std::make_shared<Session>("shared-data", nullptr);
    parallel([&](int worker) {
        const auto key = "worker/" + std::to_string(worker);
        for (int i = 0; i < iterations; ++i) {
            const auto value = std::string(1024, char('a' + worker)) + std::to_string(i);
            session->setValue(key, value);
            check(session->getValue(key) == value, "concurrent independent session data remains intact");
            session->remove(key);
            check(session->getValue(key).empty(), "session remove erases the requested value");
        }
    });
    // Exercise clear against reads and writes as well as independent-key mutations.
    parallel([&](int worker) {
        for (int i = 0; i < iterations; ++i) {
            if (worker == 0) session->clear();
            else if (worker == 1) session->remove("shared");
            else if (worker == 2) session->setValue("shared", std::string(1024, 'x'));
            else {
                const auto value = session->getValue("shared");
                check(value.empty() || value == std::string(1024, 'x'), "shared reads return a complete value copy");
            }
        }
    });
    session->clear();
    check(session->getValue("shared").empty(), "clear removes session data");
}

void expiryThreads() {
    auto session = std::make_shared<Session>("shared-expiry", nullptr);
    parallel([&](int worker) {
        for (int i = 0; i < iterations * 10; ++i) {
            if (worker % 2) session->refresh();
            else check(!session->isExpired(), "refresh/read keeps a one-hour session valid");
        }
    });
    check(Session("expired", nullptr, -1).isExpired(), "past-expiry session is expired");
}

void managerThreads(bool pointerOnly = false) {
    auto ownedStorage = std::make_unique<MemorySessionStorage>();
    auto* storage = ownedStorage.get();
    SessionManager manager(std::move(ownedStorage));
    if (!pointerOnly) {
        std::vector<std::string> ids[workers];
        parallel([&](int worker) {
            for (int i = 0; i < iterations; ++i) {
                http::HttpRequest request;
                http::HttpResponse response;
                auto session = manager.getSession(request, &response);
                check(session->getId().size() == 32, "generated session ID has expected length");
                ids[worker].push_back(session->getId());
                session->setValue("user", std::to_string(worker));
                check(storage->load(session->getId()) == session, "manager-created and auto-saved session is loadable");
                http::HttpRequest cookieRequest;
                const auto header = "Cookie: sessionId=" + session->getId();
                cookieRequest.addHeader(header.data(), header.data() + 6, header.data() + header.size());
                http::HttpResponse existingResponse;
                check(manager.getSession(cookieRequest, &existingResponse) == session, "cookie resumes the existing session");
                manager.destroySession(session->getId());
                check(!storage->load(session->getId()), "manager destroys the session");
            }
        });
        std::set<std::string> unique;
        for (const auto& workerIds : ids) unique.insert(workerIds.begin(), workerIds.end());
        check(unique.size() == workers * iterations, "concurrent RNG calls produce distinct IDs");
    }

    http::HttpRequest request;
    http::HttpResponse response;
    auto session = manager.getSession(request, &response);
    parallel([&](int worker) {
        for (int i = 0; i < iterations; ++i) {
            if (worker == 0) session->setManager(&manager);
            else if (worker == 1) check(session->getManager() == &manager, "manager pointer is visible");
            else if (worker == 2) session->setValue("shared", std::to_string(i));
            else check(storage->load(session->getId()) == session, "storage read may overlap session auto-save");
        }
    });
}
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "all";
    if (mode == "storage" || mode == "all") storageThreads();
    if (mode == "data" || mode == "all") dataThreads();
    if (mode == "expiry" || mode == "all") expiryThreads();
    if (mode == "manager" || mode == "all") managerThreads();
    if (mode == "manager-pointer") managerThreads(true);
    std::cout << "PASS: session " << mode << " (4 worker threads)\n";
}
