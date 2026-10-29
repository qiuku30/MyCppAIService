#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
using json = nlohmann::json;
using namespace std::chrono_literals;
#define LOG_INFO std::ostringstream{}
namespace muduo::net { struct Buffer { std::string body; }; }
struct Conn {
    std::mutex mutex;
    std::condition_variable changed;
    std::string wire;
    bool closed = false;
    void send(const std::string& text) { std::lock_guard<std::mutex> lock(mutex); wire += text; }
    void send(muduo::net::Buffer* buffer) { send(buffer->body); }
    void shutdown() { std::lock_guard<std::mutex> lock(mutex); closed = true; changed.notify_all(); }
    std::string wait() {
        std::unique_lock<std::mutex> lock(mutex);
        if (!changed.wait_for(lock, 2s, [&] { return closed; })) throw std::runtime_error("handler stream did not terminate");
        return wire;
    }
};
namespace http {
struct HttpRequest {
    int user_id = 1;
    bool logged_in = true;
    std::string body;
    std::shared_ptr<Conn> connection = std::make_shared<Conn>();
    const std::string& getBody() const { return body; }
    int getVersion() const { return 11; }
    std::shared_ptr<Conn> getConnection() const { return connection; }
};
struct HttpResponse {
    enum { k200Ok = 200, k400BadRequest = 400, k401Unauthorized = 401 };
    int status = 0;
    bool streaming = false;
    std::string body;
    void setStatusLine(int, int code, const std::string&) { status = code; }
    void setupSSEHeaders() { streaming = true; }
    void appendToBuffer(muduo::net::Buffer* buffer) { buffer->body = "HEAD\n"; }
    void setCloseConnection(bool) {}
    void setContentType(const std::string&) {}
    void setContentLength(size_t) {}
    void setBody(const std::string& value) { body = value; }
};
}
struct Session {
    int user;
    bool logged;
    std::string getValue(const std::string& key) const {
        if (key == "isLoggedIn") return logged ? "true" : "false";
        if (key == "userId") return std::to_string(user);
        return "fixture-user";
    }
};
struct SessionManager {
    std::shared_ptr<Session> getSession(const http::HttpRequest& request, http::HttpResponse*) {
        return std::make_shared<Session>(Session{request.user_id, request.logged_in});
    }
};
static std::atomic<int> helper_calls{0};
static std::mutex calls_mutex;
static std::vector<std::pair<int, std::string>> calls;
struct AIHelper {
    std::string chat(int uid, const std::string&, const std::string& sid, const std::string&, const std::string&) {
        std::lock_guard<std::mutex> lock(calls_mutex);
        ++helper_calls; calls.push_back({uid, sid}); return "answer";
    }
    void chatStream(int uid, const std::string& user, const std::string& sid, const std::string& question,
                    const std::string& model, std::function<void(const std::string&)> token,
                    std::function<void(const std::string&)> done, std::function<void(const std::string&)>) {
        const auto answer = chat(uid, user, sid, question, model);
        token(answer); done(answer);
    }
};
struct Server {
    std::mutex mutexForChatInformation;
    std::mutex mutexForSessionsId;
    std::unordered_map<int, std::unordered_map<std::string, std::shared_ptr<AIHelper>>> chatInformation;
    std::unordered_map<int, std::vector<std::string>> sessionsIdsMap;
    SessionManager sessions;
    SessionManager* getSessionManager() { return &sessions; }
    void packageResp(int, int status, const std::string&, bool, const std::string&, size_t,
                     const std::string& body, http::HttpResponse* response) {
        response->status = status; response->body = body;
    }
};
std::string secureId() {
    static std::atomic<unsigned> sequence{0};
    std::ostringstream out;
    out << std::hex << std::setw(32) << std::setfill('0') << ++sequence;
    return out.str();
}
class ChatSendHandler {
    Server* server_;
public:
    explicit ChatSendHandler(Server* server) : server_(server) {}
    void handle(const http::HttpRequest&, http::HttpResponse*);
};
class ChatCreateAndSendHandler {
    Server* server_;
public:
    explicit ChatCreateAndSendHandler(Server* server) : server_(server) {}
    void handle(const http::HttpRequest&, http::HttpResponse*);
};
// @CHATSENDHANDLER@
// @CHATCREATEANDSENDHANDLER@

void require(bool valid, const char* message) { if (!valid) throw std::runtime_error(message); }
int main() {
    int failures = 0, count = 0;
    auto test = [&](const char* name, auto run) {
        ++count; helper_calls = 0;
        { std::lock_guard<std::mutex> lock(calls_mutex); calls.clear(); }
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failures; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    };
    for (bool stream : {false, true}) test(stream ? "foreign_session_stream_rejected" : "foreign_session_sync_rejected", [&] {
        Server server; server.chatInformation[1]["victim-sid"] = std::make_shared<AIHelper>();
        const auto before = server.chatInformation.size();
        http::HttpRequest request; request.user_id = 2;
        request.body = json{{"question", "attack"}, {"sessionId", "victim-sid"}, {"stream", stream}}.dump();
        http::HttpResponse response; ChatSendHandler(&server).handle(request, &response);
        require(response.status == 400 && !response.streaming && helper_calls == 0 &&
                server.chatInformation.size() == before && server.chatInformation.count(2) == 0,
                "foreign id was accepted or attacker session auto-created");
    });
    test("unknown_and_empty_session_rejected", [&] {
        Server server; server.chatInformation[1]["owned"] = std::make_shared<AIHelper>();
        for (const std::string sid : {"unknown", ""}) {
            http::HttpRequest request; request.body = json{{"question", "q"}, {"sessionId", sid}}.dump();
            http::HttpResponse response; ChatSendHandler(&server).handle(request, &response);
            require(response.status == 400 && helper_calls == 0 && server.chatInformation[1].size() == 1,
                    "unknown or empty id mutated sessions");
        }
    });
    test("unauthorized_rejected_before_helper", [&] {
        Server server; http::HttpRequest request; request.logged_in = false;
        request.body = json{{"question", "q"}, {"sessionId", "owned"}}.dump();
        http::HttpResponse response; ChatSendHandler(&server).handle(request, &response);
        require(response.status == 401 && helper_calls == 0 && server.chatInformation.empty(), "unauthorized send proceeded");
    });
    for (bool stream : {false, true}) test(stream ? "owned_stream_content" : "owned_sync_content", [&] {
        Server server; server.chatInformation[2]["owned"] = std::make_shared<AIHelper>();
        http::HttpRequest request; request.user_id = 2;
        request.body = json{{"question", "q"}, {"sessionId", "owned"}, {"stream", stream}}.dump();
        http::HttpResponse response; ChatSendHandler(&server).handle(request, &response);
        if (stream) {
            const auto wire = request.connection->wait();
            require(wire.find("answer") != std::string::npos && wire.find("data: [DONE]") != std::string::npos,
                    "actual handler omitted answer or completion");
        } else require(json::parse(response.body).at("Information") == "answer", "sync answer missing");
        std::lock_guard<std::mutex> lock(calls_mutex);
        require(response.status == 200 && helper_calls == 1 && calls[0] == std::make_pair(2, std::string("owned")),
                "owned session routed to wrong helper");
    });
    for (bool stream : {false, true}) test(stream ? "create_stream_uses_new_owned_id" : "create_sync_uses_new_owned_id", [&] {
        Server server; server.chatInformation[1]["victim-sid"] = std::make_shared<AIHelper>();
        http::HttpRequest request; request.user_id = 2;
        request.body = json{{"question", "q"}, {"sessionId", "victim-sid"}, {"stream", stream}}.dump();
        http::HttpResponse response; ChatCreateAndSendHandler(&server).handle(request, &response);
        std::string wire;
        if (stream) wire = request.connection->wait();
        std::lock_guard<std::mutex> lock(calls_mutex);
        require(response.status == 200 && helper_calls == 1 && calls[0].first == 2 && calls[0].second.size() == 32 &&
                calls[0].second != "victim-sid" && server.chatInformation[1].size() == 1 &&
                server.chatInformation[2].count(calls[0].second) == 1 && server.sessionsIdsMap[2].size() == 1,
                "create accepted client session id or wrong owner");
        if (stream) require(wire.find(calls[0].second) != std::string::npos && wire.find("answer") != std::string::npos,
                            "create SSE omitted id or content");
        else require(json::parse(response.body).at("sessionId") == calls[0].second, "create sync id mismatch");
    });
    std::cout << "HANDLER_FAILURES=" << failures << '/' << count << '\n';
    return failures ? 1 : 0;
}
