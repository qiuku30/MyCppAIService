#include "http/HttpServer.h"
#include <chrono>
#include <map>
#include <mutex>
#include <sstream>

namespace {
struct Observation {
    std::mutex mutex;
    std::map<std::string, std::weak_ptr<http::session::Session>> sessions;
    std::shared_ptr<http::session::Session> held;
};
std::unique_ptr<http::session::SessionManager> manager() {
    return std::make_unique<http::session::SessionManager>(
        std::make_unique<http::session::MemorySessionStorage>(2),
        std::chrono::milliseconds(25), 2);
}
void reply(const http::HttpRequest& req, http::HttpResponse* response, const std::string& body) {
    response->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
    response->setCloseConnection(true);
    response->setContentType("application/json");
    response->setBody(body);
}
}

int main(int argc, char** argv) {
    muduo::Logger::setLogLevel(muduo::Logger::INFO);
    http::HttpServer server(argc > 1 ? std::stoi(argv[1]) : 8118, "session-network-fixture");
    server.setThreadNum(2);
    server.setSessionManager(manager());
    auto observed = std::make_shared<Observation>();
    server.Get("/session", [&server, observed](const http::HttpRequest& req, http::HttpResponse* response) {
        auto session = server.getSessionManager()->getSession(req, response);
        if (session->getValue("value").empty()) session->setValue("value", "retained");
        {
            std::lock_guard<std::mutex> lock(observed->mutex);
            observed->sessions[session->getId()] = session;
        }
        reply(req, response, "{\"ok\":true,\"value\":\"" + session->getValue("value") + "\"}");
    });
    server.Get("/drop", [&server](const http::HttpRequest& req, http::HttpResponse* response) {
        auto session = server.getSessionManager()->getSession(req, response);
        server.getSessionManager()->destroySession(session->getId());
        reply(req, response, "{\"dropped\":true}");
    });
    server.Get("/observe", [observed](const http::HttpRequest& req, http::HttpResponse* response) {
        std::size_t live = 0, expired = 0;
        {
            std::lock_guard<std::mutex> lock(observed->mutex);
            for (const auto& item : observed->sessions) {
                if (item.second.expired()) ++expired; else ++live;
            }
        }
        reply(req, response, "{\"live\":" + std::to_string(live) + ",\"expired\":" + std::to_string(expired) + "}");
    });
    server.Get("/hold", [&server, observed](const http::HttpRequest& req, http::HttpResponse* response) {
        auto session = server.getSessionManager()->getSession(req, response);
        {
            std::lock_guard<std::mutex> lock(observed->mutex);
            observed->held = session;
        }
        reply(req, response, "{\"held\":true}");
    });
    server.Get("/replace", [&server, observed](const http::HttpRequest& req, http::HttpResponse* response) {
        std::shared_ptr<http::session::Session> held;
        {
            std::lock_guard<std::mutex> lock(observed->mutex);
            held = std::move(observed->held);
        }
        server.setSessionManager(manager());
        const bool detached = held && held->getManager() == nullptr;
        if (held) held->setValue("after", "safe");
        const bool safe = detached && held->getValue("after") == "safe";
        reply(req, response, safe ? "{\"retained_safe\":true}" : "{\"retained_safe\":false}");
    });
    server.start();
}
