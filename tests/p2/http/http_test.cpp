#include "http/HttpServer.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
unsigned checks = 0;
unsigned failures = 0;
void check(bool condition, const std::string& name) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}
std::string serialize(const http::HttpResponse& response) {
    muduo::net::Buffer buffer;
    response.appendToBuffer(&buffer);
    return buffer.retrieveAllAsString();
}
bool rejected(const std::string& method, const std::string& headers) {
    http::HttpContext context;
    muduo::net::Buffer buffer;
    buffer.append(method + " / HTTP/1.1\r\n" + headers + "\r\nxy");
    return !context.parseRequest(&buffer, muduo::Timestamp());
}
void parser() {
    {
        http::HttpContext context;
        muduo::net::Buffer buffer;
        buffer.append("POST / HTTP/1.1\r\ncontent-length: 2\r\ncontent-type: application/json\r\ncookie: sessionId=abc\r\n\r\n{}");
        check(context.parseRequest(&buffer, muduo::Timestamp()) && context.gotAll(), "lowercase Content-Length parses POST");
        check(context.request().getBody() == "{}", "lowercase Content-Length consumes body");
        check(context.request().getHeader("Content-Type") == "application/json", "lowercase content-type lookup");
        check(context.request().getHeader("cOoKiE") == "sessionId=abc", "case-insensitive Cookie lookup");
    }
    for (const auto& value : {"2junk", "+2", "-0", "2.0", "2 0", "2, 2", "", "2\v"}) {
        check(rejected("POST", "Content-Length: " + std::string(value) + "\r\n"), "reject non-decimal length [" + std::string(value) + "]");
    }
    check(rejected("POST", "Content-Length: 18446744073709551616\r\n"), "reject uint64 overflow length");
    check(rejected("POST", "Content-Length: 10485761\r\n"), "reject body over 10 MiB");
    check(rejected("POST", "Content-Length: 1\r\nContent-Length: 2\r\n"), "reject conflicting duplicate Content-Length");
    check(rejected("POST", "Content-Length: 1\r\ncOnTeNt-LeNgTh: 2\r\n"), "reject mixed-case conflicting lengths");
    check(rejected("GET", "Content-Length: 2junk\r\n"), "validate framing for GET");
    check(rejected("GET", "Transfer-Encoding: chunked\r\n"), "reject unsupported GET transfer framing");
    check(rejected("POST", "Content-Length: 2\r\ntRaNsFeR-EnCoDiNg: chunked\r\n"), "reject Transfer-Encoding with Content-Length");
    check(rejected("GET", "Transfer-Encoding:\r\n"), "reject empty transfer framing");
    check(rejected("POST", "Content-Length : 2\r\nContent-Length: 2\r\n"), "reject whitespace before header colon");
    {
        http::HttpContext context;
        muduo::net::Buffer buffer;
        buffer.append("POST / HTTP/1.1\r\nContent-Length: 2\r\ncontent-length: 2\r\n\r\nxy");
        check(context.parseRequest(&buffer, muduo::Timestamp()) && context.gotAll() && context.request().getBody() == "xy", "accept identical duplicate lengths");
    }
    {
        http::HttpContext context;
        muduo::net::Buffer buffer;
        buffer.append("POST / HTTP/1.1\r\nContent-Length: 10485760\r\n\r\n" + std::string(10485760, 'x'));
        check(context.parseRequest(&buffer, muduo::Timestamp()) && context.gotAll() && context.request().getBody().size() == 10485760, "accept exact 10 MiB body");
    }
    {
        http::HttpContext context;
        muduo::net::Buffer buffer;
        buffer.append("GET /first HTTP/1.1\r\nContent-Length: 2\r\n\r\nxyGET /second HTTP/1.1\r\n\r\n");
        check(context.parseRequest(&buffer, muduo::Timestamp()) && context.gotAll() && context.request().getBody() == "xy", "GET consumes declared body before next request");
        context.reset();
        check(context.parseRequest(&buffer, muduo::Timestamp()) && context.gotAll() && context.request().path() == "/second", "following request stays aligned");
        check(context.request().getBody().empty() && context.request().contentLength() == 0, "reset preserves P1 body isolation");
    }
    {
        http::HttpContext context;
        muduo::net::Buffer buffer;
        buffer.append("PUT / HTTP/1.1\r\nCoNtEnT-LeNgTh: \t2 \t\r\n\r\nx");
        check(context.parseRequest(&buffer, muduo::Timestamp()) && !context.gotAll(), "fragmented body waits for declared length");
        buffer.append("y");
        check(context.parseRequest(&buffer, muduo::Timestamp()) && context.gotAll() && context.request().getBody() == "xy", "fragmented mixed-case body completes");
    }
}
void response() {
    {
        http::HttpResponse response;
        const auto bytes = serialize(response);
        check(bytes == "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\nContent-Length: 0\r\n\r\n", "default response has complete 500 framing");
    }
    {
        http::HttpResponse response(false);
        response.setStatusCode(http::HttpResponse::k404NotFound);
        response.setBody("missing");
        const auto bytes = serialize(response);
        check(bytes.find("HTTP/1.1 404 Not Found\r\n") == 0, "code-only status has version and canonical reason");
        check(bytes.find("Content-Length: 7\r\n") != std::string::npos, "keep-alive response gets body length");
        check(bytes.find("Connection: Keep-Alive\r\n") != std::string::npos, "keep-alive response retains connection policy");
    }
    {
        http::HttpResponse response(false);
        response.setStatusLine("HTTP/1.0", http::HttpResponse::k200Ok, "Custom OK");
        response.setContentLength(1);
        response.addHeader("content-length", "99");
        response.addHeader("Transfer-Encoding", "chunked");
        response.setBody("three");
        const auto bytes = serialize(response);
        check(bytes.find("HTTP/1.0 200 Custom OK\r\n") == 0, "explicit version and reason are preserved");
        check(bytes.find("Content-Length: 5\r\n") != std::string::npos && bytes.find("Content-Length: 1\r\n") == std::string::npos && bytes.find("content-length:") == std::string::npos, "serialization derives one correct body length");
        check(bytes.find("Transfer-Encoding:") == std::string::npos, "non-streaming response does not advertise an unencoded transfer");
    }
    {
        http::HttpResponse response(false);
        response.setStatusCode(http::HttpResponse::k204NoContent);
        response.setContentLength(3);
        response.setBody("bad");
        const auto bytes = serialize(response);
        check(bytes.find("HTTP/1.1 204 No Content\r\n") == 0, "204 has a complete status line");
        check(bytes.find("Content-Length:") == std::string::npos && bytes.substr(bytes.find("\r\n\r\n") + 4).empty(), "204 omits body and Content-Length per RFC 9110");
    }
    {
        http::HttpResponse response;
        response.setStatusCode(http::HttpResponse::k200Ok);
        response.setupSSEHeaders();
        const auto bytes = serialize(response);
        const auto first = bytes.find("Connection:");
        check(first != std::string::npos && bytes.find("Connection:", first + 1) == std::string::npos, "streaming headers contain one Connection field");
        check(bytes.find("Content-Length:") == std::string::npos && response.isStreaming(), "streaming response has no synthesized body length");
    }
}
void cors() {
    for (bool allowed : {true, false}) {
        http::HttpContext context;
        muduo::net::Buffer buffer;
        buffer.append("OPTIONS / HTTP/1.0\r\norigin: https://" + std::string(allowed ? "allowed" : "denied") + ".example\r\n\r\n");
        check(context.parseRequest(&buffer, muduo::Timestamp()) && context.gotAll(), "parse CORS request");
        http::middleware::CorsConfig config;
        config.allowedOrigins = {"https://allowed.example"};
        http::middleware::CorsMiddleware middleware(config);
        try { middleware.before(context.request()); check(false, "CORS stops request handling"); }
        catch (const http::HttpResponse& response) {
            const auto bytes = serialize(response);
            check(bytes.find(allowed ? "HTTP/1.0 204 No Content\r\n" : "HTTP/1.0 403 Forbidden\r\n") == 0, allowed ? "allowed CORS status line uses request version" : "denied CORS status line uses request version");
            if (allowed) check(bytes.find("Content-Length:") == std::string::npos && bytes.find("Access-Control-Allow-Origin: https://allowed.example\r\n") != std::string::npos, "allowed CORS has headers and zero-content framing");
            else check(bytes.find("Content-Length: 0\r\n") != std::string::npos, "denied CORS has complete zero-body framing");
        }
    }
}
muduo::net::TcpConnectionPtr exchange(const std::string& raw) {
    auto connection = std::make_shared<muduo::net::TcpConnection>();
    auto* transport = muduo::net::TcpServer::latest;
    transport->connect(connection);
    muduo::net::Buffer buffer;
    buffer.append(raw);
    transport->receive(connection, &buffer);
    return connection;
}
void server() {
    {
        http::HttpServer server(0, "fixture");
        const auto conn = exchange("GET /missing HTTP/1.0\r\n\r\n");
        check(conn->sent == "HTTP/1.0 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n" && conn->shutdownCalled, "default route returns complete request-version 404");
    }
    {
        http::HttpServer server(0, "fixture");
        server.Get("/error", [](const http::HttpRequest&, http::HttpResponse* response) {
            response->setStatusLine("HTTP/1.1", http::HttpResponse::k200Ok, "Old reason");
            response->setContentLength(99);
            response->setStreaming(true);
            throw std::runtime_error("handler failed");
        });
        const auto conn = exchange("GET /error HTTP/1.0\r\n\r\n");
        check(conn->sent.find("HTTP/1.0 500 Internal Server Error\r\n") == 0, "handler exception returns complete 500 after partial response");
        check(conn->sent.find("Content-Length: 14\r\n") != std::string::npos && conn->sent.substr(conn->sent.find("\r\n\r\n") + 4) == "handler failed", "500 length matches actual error body");
    }
    {
        http::HttpServer server(0, "fixture");
        server.Get("/ok", [](const http::HttpRequest&, http::HttpResponse* response) { response->setStatusCode(http::HttpResponse::k200Ok); response->setBody("ok"); });
        const auto keep = exchange("GET /ok HTTP/1.0\r\nconnection: keep-alive\r\n\r\n");
        check(keep->sent.find("HTTP/1.0 200 OK\r\n") == 0 && !keep->shutdownCalled, "HTTP/1.0 lowercase keep-alive is honored");
        check(keep->sent.find("Content-Length: 2\r\n") != std::string::npos, "keep-alive route response has exact body length");
        const auto close = exchange("GET /ok HTTP/1.1\r\nConnection: keep-alive, ClOsE\r\n\r\n");
        check(close->shutdownCalled && close->sent.find("Connection: close\r\n") != std::string::npos, "case-insensitive close token overrides keep-alive");
    }
    {
        http::HttpServer server(0, "fixture");
        const auto conn = exchange("POST / HTTP/1.1\r\nContent-Length: 2junk\r\n\r\nxy");
        check(conn->sent == "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n" && conn->shutdownCalled, "invalid framing returns complete 400 and closes");
    }
}
void lifetime() {
    const auto beforeConstruction = muduo::net::TcpServer::constructionViolations;
    const auto beforeDestruction = muduo::net::TcpServer::destructionViolations;
    { http::HttpServer server(0, "lifetime-fixture"); }
    check(muduo::net::TcpServer::constructionViolations == beforeConstruction, "EventLoop lifetime starts before TcpServer constructor");
    check(muduo::net::TcpServer::destructionViolations == beforeDestruction, "EventLoop outlives TcpServer destructor");
}
}
void dispatch() {
    const auto connect = [] {
        auto conn = std::make_shared<muduo::net::TcpConnection>();
        muduo::net::TcpServer::latest->connect(conn);
        return conn;
    };
    const auto receive = [](const muduo::net::TcpConnectionPtr& conn, muduo::net::Buffer& buffer) {
        muduo::net::TcpServer::latest->receive(conn, &buffer);
    };
    const std::string get = "GET /ok HTTP/1.1\r\n\r\n";
    {
        http::HttpServer server(0, "coalesced");
        std::vector<std::string> bodies;
        server.Post("/ok", [&bodies](const http::HttpRequest& req, http::HttpResponse* resp) {
            bodies.push_back(req.getBody()); resp->setStatusCode(http::HttpResponse::k200Ok); resp->setBody(req.getBody());
        });
        auto conn = connect();
        muduo::net::Buffer buffer;
        buffer.append("POST /ok HTTP/1.1\r\nContent-Length: 1\r\n\r\nAPOST /ok HTTP/1.1\r\nContent-Length: 1\r\n\r\nB");
        receive(conn, buffer);
        check(bodies == std::vector<std::string>({"A", "B"}), "coalesced bodies dispatch once each in wire order");
        check(buffer.readableBytes() == 0, "coalesced complete requests drain without another socket read");
        check(!conn->shutdownCalled && conn->sent.find("\r\n\r\nAHTTP/1.1 200 OK\r\n") != std::string::npos, "coalesced keep-alive responses remain framed and ordered");
    }
    {
        http::HttpServer server(0, "partial-successor");
        unsigned calls = 0;
        server.Get("/ok", [&calls](const http::HttpRequest&, http::HttpResponse* resp) { ++calls; resp->setStatusCode(http::HttpResponse::k200Ok); });
        auto conn = connect();
        muduo::net::Buffer buffer;
        buffer.append(get + "GET /ok HTTP/1.1\r\nConnec");
        receive(conn, buffer);
        check(calls == 1, "partial successor waits after dispatching complete predecessor");
        receive(conn, buffer);
        check(calls == 1, "repeated partial successor callback returns without dispatch or spin");
        buffer.append("tion: keep-alive\r\n\r\n");
        receive(conn, buffer);
        check(calls == 2 && buffer.readableBytes() == 0, "partial successor completes on later bytes");
    }
    {
        http::HttpServer server(0, "partial-body");
        std::vector<std::string> bodies;
        server.Post("/ok", [&bodies](const http::HttpRequest& req, http::HttpResponse* resp) { bodies.push_back(req.getBody()); resp->setStatusCode(http::HttpResponse::k200Ok); });
        auto conn = connect();
        muduo::net::Buffer buffer;
        buffer.append("POST /ok HTTP/1.1\r\nContent-Length: 3\r\n\r\nx");
        receive(conn, buffer);
        receive(conn, buffer);
        check(bodies.empty() && conn->sent.empty(), "single incomplete body returns without dispatch or spin");
        buffer.append("yz");
        receive(conn, buffer);
        check(bodies == std::vector<std::string>({"xyz"}), "single incomplete body is retained until complete");
    }
    for (const std::string stop : {"close", "close-override", "handler-close", "stream", "error", "invalid"}) {
        http::HttpServer server(0, "stop-" + stop);
        unsigned firstCalls = 0, laterCalls = 0;
        server.Get("/first", [&firstCalls, &stop](const http::HttpRequest& req, http::HttpResponse* resp) {
            ++firstCalls;
            if (stop == "error") throw std::runtime_error("failed");
            resp->setStatusCode(http::HttpResponse::k200Ok);
            if (stop == "stream") resp->setupSSEHeaders();
            if (stop == "close-override") resp->setCloseConnection(false);
            if (stop == "handler-close") req.getConnection()->shutdown();
        });
        server.Get("/ok", [&laterCalls](const http::HttpRequest&, http::HttpResponse* resp) { ++laterCalls; resp->setStatusCode(http::HttpResponse::k200Ok); });
        auto conn = connect();
        muduo::net::Buffer buffer;
        if (stop == "invalid") buffer.append("POST /first HTTP/1.1\r\nContent-Length: 2junk\r\n\r\nxy" + get);
        else buffer.append("GET /first HTTP/1.1\r\n" + std::string((stop == "close" || stop == "close-override") ? "Connection: close\r\n" : "") + "\r\n" + get);
        receive(conn, buffer);
        check(firstCalls == (stop == "invalid" ? 0u : 1u) && laterCalls == 0, stop + " stops buffered later request");
        check(buffer.readableBytes() == 0, stop + " discards terminal buffered HTTP input");
        const auto firstResponse = conn->sent;
        receive(conn, buffer);
        check(laterCalls == 0 && conn->sent == firstResponse, stop + " stays stopped on repeated message callback");
        muduo::net::Buffer newInput;
        newInput.append(get);
        receive(conn, newInput);
        check(laterCalls == 0 && conn->sent == firstResponse, stop + " stays stopped on future input");
        check(newInput.readableBytes() == 0, stop + " does not retain future terminal input");
        if (stop == "stream") check(!conn->shutdownCalled && conn->sent.empty(), "SSE takeover keeps handler ownership without framework send or shutdown");
        else check(conn->shutdownCalled, stop + " closes the connection");
    }
    {
        http::HttpServer server(0, "malformed-successor");
        unsigned calls = 0;
        server.Get("/ok", [&calls](const http::HttpRequest&, http::HttpResponse* resp) { ++calls; resp->setStatusCode(http::HttpResponse::k200Ok); });
        auto conn = connect();
        muduo::net::Buffer buffer;
        buffer.append(get + "INVALID / HTTP/1.1\r\n\r\n" + get);
        receive(conn, buffer);
        check(calls == 1 && conn->shutdownCalled && conn->sent.find("HTTP/1.1 400 Bad Request\r\n") != std::string::npos, "malformed successor gets one 400 after preceding response");
        const auto bytes = conn->sent;
        receive(conn, buffer);
        check(calls == 1 && conn->sent == bytes, "malformed successor permanently stops subsequent dispatch");
    }
}

int main(int argc, char** argv) {
    const std::string selected = argc > 1 ? argv[1] : "all";
    if (selected == "all" || selected == "parser") parser();
    if (selected == "all" || selected == "response") response();
    if (selected == "all" || selected == "cors") cors();
    if (selected == "all" || selected == "server") server();
    if (selected == "all" || selected == "lifetime") lifetime();
    if (selected == "all" || selected == "dispatch") dispatch();
    std::cout << "HTTP checks: " << checks - failures << '/' << checks << " passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
