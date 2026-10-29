#include "http/HttpContext.h"
#include <iostream>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

void connectionOwnership() {
    auto connection = std::make_shared<muduo::net::TcpConnection>();
    auto context = std::make_shared<http::HttpContext>();
    connection->context = context;
    std::weak_ptr<muduo::net::TcpConnection> observer = connection;
    context->request().setConnection(connection);
    check(context->request().getConnection() == connection, "live connection is available to handlers");
    context.reset();
    connection.reset();
    check(observer.expired(), "connection/context/request cycle must not retain a disconnected connection");

    http::HttpRequest request;
    {
        auto temporary = std::make_shared<muduo::net::TcpConnection>();
        request.setConnection(temporary);
    }
    check(!request.getConnection(), "request returns an empty pointer after connection lifetime ends");
}

void resetRequest() {
    http::HttpContext context;
    muduo::net::Buffer buffer;
    auto connection = std::make_shared<muduo::net::TcpConnection>();
    context.request().setConnection(connection);
    context.request().setPathParameters("id", "first");
    buffer.append("POST /first?q=secret HTTP/1.1\r\nContent-Length: 6\r\nCookie: secret\r\n\r\nSECRET");
    check(context.parseRequest(&buffer, {}) && context.gotAll(), "first request parses");
    check(context.request().getBody() == "SECRET", "first request owns its body");
    context.reset();
    check(context.request().getBody().empty(), "reset clears previous request body");
    check(context.request().contentLength() == 0, "reset clears previous content length");
    check(!context.request().getConnection(), "reset clears previous connection");
    check(context.request().headers().empty(), "reset clears headers");
    check(context.request().method() == http::HttpRequest::kInvalid, "reset clears method");
    check(context.request().path().empty(), "reset clears path");
    check(context.request().getPathParameters("id").empty(), "reset clears path parameters");
    check(context.request().getQueryParameters("q").empty(), "reset clears query parameters");
    check(context.request().getVersion() == "Unknown", "reset clears version");
    buffer.append("POST /second HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
    check(context.parseRequest(&buffer, {}) && context.gotAll(), "second empty-body request parses");
    check(context.request().getBody().empty(), "keep-alive second request cannot see previous body");
    check(context.request().contentLength() == 0, "second request has zero length");
}

void swapRequest() {
    http::HttpRequest first, second;
    auto firstConnection = std::make_shared<muduo::net::TcpConnection>();
    auto secondConnection = std::make_shared<muduo::net::TcpConnection>();
    first.setBody("one"); first.setContentLength(3); first.setConnection(firstConnection);
    second.setBody("second"); second.setContentLength(6); second.setConnection(secondConnection);
    first.swap(second);
    check(first.getBody() == "second" && second.getBody() == "one", "swap exchanges both bodies");
    check(first.contentLength() == 6 && second.contentLength() == 3, "swap exchanges both content lengths");
    check(first.getConnection() == secondConnection && second.getConnection() == firstConnection,
          "swap exchanges both connection observers");
}
}

int main() {
    connectionOwnership(); resetRequest(); swapRequest();
    if (failures) return 1;
    std::cout << "PASS: HTTP connection lifetime, full swap, keep-alive request reset\n";
}
