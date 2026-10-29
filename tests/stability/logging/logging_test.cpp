#include "http/HttpServer.h"
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <stdexcept>
#include <thread>

namespace {
unsigned checks = 0, failures = 0;
void check(bool value, const std::string& name) {
    ++checks; if (!value) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}
std::string logs() {
    std::string text; for (const auto& line : muduo::takeLogs()) text += line + '\n'; return text;
}
muduo::net::TcpConnectionPtr exchange(const std::string& request) {
    auto conn = std::make_shared<muduo::net::TcpConnection>();
    auto* server = muduo::net::TcpServer::latest;
    server->connect(conn); muduo::net::Buffer buf; buf.append(request); server->receive(conn, &buf); return conn;
}
void privateLogs(const std::string& text, const std::vector<std::string>& markers) {
    for (const auto& marker : markers) check(text.find(marker) == std::string::npos, "private marker excluded: " + marker);
}
void metadata(const std::string& text, const std::string& method, const std::string& route,
              int status, size_t bytes, bool streaming = false) {
    const std::regex event("http_request request_id=[1-9][0-9]* method=" + method +
        " route=\"" + route + "\" status=" + std::to_string(status) +
        " elapsed_us=[0-9]+ event=" + (streaming ? "stream_open" : "response") +
        (streaming ? "" : " response_bytes=" + std::to_string(bytes)) + "\\n");
    check(std::regex_search(text, event), "safe metadata for " + route);
    if (streaming) check(text.find("response_bytes=") == std::string::npos, "stream open does not claim completed response bytes");
}
struct ThrowAfter : http::middleware::Middleware {
    void before(http::HttpRequest&) override {}
    void after(http::HttpResponse&) override { throw std::runtime_error("AFTER_EXCEPTION_SECRET"); }
};
struct ObjectHandler : http::router::RouterHandler {
    void handle(const http::HttpRequest&, http::HttpResponse* resp) override { resp->setStatusCode(http::HttpResponse::k200Ok); resp->setBody("object"); }
};
}
int main() {
    std::string evidence;
    {
        http::HttpServer server(0, "privacy");
        server.Post("/chat", [](const http::HttpRequest&, http::HttpResponse* resp) {
            resp->setStatusCode(http::HttpResponse::k200Ok); resp->addHeader("Set-Cookie", "COOKIE_RESPONSE_SECRET");
            resp->addHeader("Authorization", "AUTH_RESPONSE_SECRET"); resp->setBody("MODEL_OUTPUT_SECRET");
        });
        auto conn = exchange("POST /chat?query=QUERY_SECRET HTTP/1.1\r\nAuthorization: AUTH_REQUEST_SECRET\r\nCookie: COOKIE_REQUEST_SECRET\r\nContent-Length: 21\r\n\r\nPASSWORD_BODY_SECRET!");
        check(conn->sent == "HTTP/1.1 200 OK\r\nConnection: Keep-Alive\r\nContent-Length: 19\r\nAuthorization: AUTH_RESPONSE_SECRET\r\nSet-Cookie: COOKIE_RESPONSE_SECRET\r\n\r\nMODEL_OUTPUT_SECRET", "ordinary response bytes preserved");
        auto text = logs(); evidence += text;
        privateLogs(text, {"COOKIE_RESPONSE_SECRET", "AUTH_RESPONSE_SECRET", "MODEL_OUTPUT_SECRET", "QUERY_SECRET", "AUTH_REQUEST_SECRET", "COOKIE_REQUEST_SECRET", "PASSWORD_BODY_SECRET"});
        metadata(text, "POST", "/chat", 200, conn->sent.size());
    }
    {
        http::HttpServer server(0, "unmatched");
        auto conn = exchange("GET /UNMATCHED_SECRET%0aFORGED?query=UNMATCHED_QUERY_SECRET HTTP/1.1\r\n\r\n");
        auto text = logs(); evidence += text;
        privateLogs(text, {"UNMATCHED_SECRET", "UNMATCHED_QUERY_SECRET", "FORGED"});
        check(conn->sent == "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n", "unmatched wire behavior preserved");
        metadata(text, "GET", "unmatched", 404, conn->sent.size());
    }
    {
        http::HttpServer server(0, "dynamic");
        server.addRoute(http::HttpRequest::kGet, "/users/:id", [](const http::HttpRequest& req, http::HttpResponse* resp) {
            resp->setStatusCode(http::HttpResponse::k200Ok); resp->setBody(req.getPathParameters("param1"));
        });
        auto conn = exchange("GET /users/DYNAMIC_SECRET?query=DYNAMIC_QUERY_SECRET HTTP/1.1\r\n\r\n");
        auto text = logs(); evidence += text;
        privateLogs(text, {"DYNAMIC_SECRET", "DYNAMIC_QUERY_SECRET"});
        check(conn->sent.substr(conn->sent.find("\r\n\r\n") + 4) == "DYNAMIC_SECRET", "dynamic path parameter still delivered");
        metadata(text, "GET", "/users/:id", 200, conn->sent.size());
    }
    {
        http::HttpServer server(0, "objects");
        server.Get("/object", std::make_shared<ObjectHandler>());
        server.addRoute(http::HttpRequest::kGet, "/object/:id", std::make_shared<ObjectHandler>());
        for (const auto& path : {"/object", "/object/PARAM_SECRET"}) {
            auto conn = exchange(std::string("GET ") + path + " HTTP/1.1\r\n\r\n"); auto text = logs(); evidence += text;
            metadata(text, "GET", path == std::string("/object") ? "/object" : "/object/:id", 200, conn->sent.size());
        }
    }
    {
        http::HttpServer server(0, "error");
        server.Get("/error", [](const http::HttpRequest&, http::HttpResponse*) { throw std::runtime_error("HANDLER_EXCEPTION_SECRET"); });
        auto conn = exchange("GET /error HTTP/1.0\r\n\r\n"); auto text = logs(); evidence += text;
        check(conn->sent.find("HTTP/1.0 500 Internal Server Error\r\n") == 0 && conn->sent.find("HANDLER_EXCEPTION_SECRET") != std::string::npos, "existing client exception behavior preserved");
        privateLogs(text, {"HANDLER_EXCEPTION_SECRET"}); metadata(text, "GET", "error", 500, conn->sent.size());
    }
    {
        http::HttpServer server(0, "callback-error");
        server.setHttpCallback([](const http::HttpRequest&, http::HttpResponse*) { throw std::runtime_error("CALLBACK_EXCEPTION_SECRET"); });
        auto conn = exchange("GET /CALLBACK_PATH_SECRET HTTP/1.1\r\n\r\n"); auto text = logs(); evidence += text;
        privateLogs(text, {"CALLBACK_EXCEPTION_SECRET", "CALLBACK_PATH_SECRET"});
        check(conn->sent == "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n", "custom callback exception wire preserved");
        metadata(text, "GET", "error", 400, conn->sent.size());
    }
    {
        http::HttpServer server(0, "after-error"); server.addMiddleware(std::make_shared<ThrowAfter>());
        server.Get("/ok", [](const http::HttpRequest&, http::HttpResponse* resp) { resp->setStatusCode(http::HttpResponse::k200Ok); });
        auto conn = exchange("GET /ok HTTP/1.1\r\n\r\n"); auto text = logs(); evidence += text;
        privateLogs(text, {"AFTER_EXCEPTION_SECRET"}); metadata(text, "GET", "/ok", 200, conn->sent.size());
    }
    {
        http::HttpServer server(0, "preflight"); http::middleware::CorsConfig config; config.allowedOrigins = {"https://allowed.example"};
        server.addMiddleware(std::make_shared<http::middleware::CorsMiddleware>(config));
        auto conn = exchange("OPTIONS /CORS_PATH_SECRET HTTP/1.1\r\nOrigin: https://ORIGIN_SECRET.example\r\n\r\n"); auto text = logs(); evidence += text;
        privateLogs(text, {"CORS_PATH_SECRET", "ORIGIN_SECRET"}); metadata(text, "OPTIONS", "preflight", 403, conn->sent.size());
    }
    {
        http::HttpServer server(0, "stream");
        const std::string wire = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\ndata: SSE_MODEL_SECRET\n\n";
        server.setHttpCallback([&wire](const http::HttpRequest& req, http::HttpResponse* resp) { resp->setStatusCode(http::HttpResponse::k200Ok); resp->setupSSEHeaders(); req.getConnection()->send(wire); });
        auto conn = exchange("GET /STREAM_PATH_SECRET HTTP/1.1\r\n\r\nGET /ignored HTTP/1.1\r\n\r\n"); auto text = logs(); evidence += text;
        check(conn->sent == wire && !conn->shutdownCalled, "stream wire and connection takeover preserved");
        privateLogs(text, {"SSE_MODEL_SECRET", "STREAM_PATH_SECRET"}); metadata(text, "GET", "callback", 200, 0, true);
    }
    {
        http::HttpServer server(0, "labels");
        const std::string label = "/quoted\"\\\t" + std::string(180, 'x');
        server.Get(label, [](const http::HttpRequest&, http::HttpResponse* resp) { resp->setStatusCode(http::HttpResponse::k200Ok); });
        auto conn = exchange("GET " + label + " HTTP/1.1\r\n\r\n"); auto text = logs(); evidence += text;
        check(conn->sent.find("200 OK") != std::string::npos, "registered quoted label dispatches");
        check(text.find("route=\"/quoted\\\"\\\\\\x09") != std::string::npos, "registered label quote, slash and control bytes escaped");
        check(text.find(std::string(129, 'x')) == std::string::npos && text.find("...\" status=200") != std::string::npos, "registered labels bounded");
    }
    {
        http::HttpServer server(0, "concurrent");
        server.Get("/ok", [](const http::HttpRequest&, http::HttpResponse* resp) { resp->setStatusCode(http::HttpResponse::k200Ok); });
        std::vector<std::thread> workers;
        for (unsigned i = 0; i < 24; ++i) workers.emplace_back([] { exchange("GET /ok HTTP/1.1\r\n\r\n"); });
        for (auto& thread : workers) thread.join();
        auto text = logs(); evidence += text; std::set<std::string> ids; unsigned count = 0;
        const std::regex id("http_request request_id=([1-9][0-9]*) ");
        for (std::sregex_iterator it(text.begin(), text.end(), id), end; it != end; ++it) { ++count; ids.insert((*it)[1]); }
        check(count == 24 && ids.size() == 24, "concurrent request IDs are numeric, unique and complete");
    }
    std::cout << evidence << "checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
