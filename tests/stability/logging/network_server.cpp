#include "http/HttpServer.h"
#include <muduo/net/Buffer.h>
#include <stdexcept>

// Real muduo network/serializer fixture. The values below are synthetic;
// no database, provider or real login credentials are involved.
int main(int argc, char** argv) {
    muduo::Logger::setLogLevel(muduo::Logger::INFO);
    http::HttpServer server(argc > 1 ? std::stoi(argv[1]) : 8118, "log-network-fixture");
    server.setThreadNum(2);
    server.Post("/private", [](const http::HttpRequest& req, http::HttpResponse* response) {
        response->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        response->setCloseConnection(true);
        response->addHeader("Set-Cookie", "sessionId=FAKE_COOKIE_PRIVATE; Path=/; HttpOnly");
        response->addHeader("Authorization", "Bearer FAKE_AUTH_RESPONSE_PRIVATE");
        response->setBody("FAKE_RESPONSE_BODY_PRIVATE");
    });
    server.Get("/failure", [](const http::HttpRequest&, http::HttpResponse*) {
        throw std::runtime_error("FAKE_EXCEPTION_PRIVATE");
    });
    server.Get("/stream", [](const http::HttpRequest& req, http::HttpResponse* response) {
        response->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        response->setupSSEHeaders();
        muduo::net::Buffer header;
        response->appendToBuffer(&header);
        auto connection = req.getConnection();
        connection->send(&header);
        connection->send("data: FAKE_STREAM_BODY_PRIVATE\n\ndata: [DONE]\n\n");
        connection->shutdown();
    });
    server.start();
}
