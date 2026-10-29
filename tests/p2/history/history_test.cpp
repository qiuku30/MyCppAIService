#include "AIApps/ChatServer/include/AIUtil/ChatMessage.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <functional>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using Messages = std::vector<std::pair<std::string, long long>>;
static int modelCalls = 0, publishCalls = 0;
static bool queueAccept = true;
static std::vector<Messages> modelPayloads;
static std::vector<std::string> localKeys;
std::string secureId() { return std::string(32, 'a'); }
struct MQManager {
    static MQManager& instance() { static MQManager value; return value; }
    bool publish(const std::string&, const std::string&) { ++publishCalls; return queueAccept; }
};
struct AIStrategy {
    bool isMCPModel = false, local = false;
    bool isLocal() const { return local; }
    json buildRequest(const Messages& history) const { modelPayloads.push_back(history); return "payload"; }
    json buildStreamRequest(const Messages& history) const { return buildRequest(history); }
    std::string parseResponse(const json& value) const { return value.get<std::string>(); }
};
struct StrategyFactory {
    static StrategyFactory& instance() { static StrategyFactory value; return value; }
    std::shared_ptr<AIStrategy> create(const std::string& code) {
        auto result = std::make_shared<AIStrategy>(); result->local = code == "5"; return result;
    }
};
struct AIToolCall { bool isToolCall; std::string toolName; json args; };
struct AIConfig {
    bool loadFromFile(const std::string&) { return true; }
    std::string buildPrompt(const std::string& value) { return value; }
    AIToolCall parseAIResponse(const std::string&) { return {false, "", nullptr}; }
    std::string buildToolResultPrompt(const std::string& value, const std::string&, const json&, const json&) { return value; }
};
struct AIToolRegistry { json invoke(const std::string&, const json&) { return "tool result"; } };
struct GemmaInferencePool {
    static GemmaInferencePool& instance() { static GemmaInferencePool value; return value; }
    bool isLoaded() const { return true; }
    std::string generateSyncFromHistory(const std::string& key, const Messages& history) {
        ++modelCalls; localKeys.push_back(key); modelPayloads.push_back(history); return "answer";
    }
    void generateFromHistory(const std::string& key, const Messages& history,
                             std::function<void(const std::string&)> token) { token(generateSyncFromHistory(key, history)); }
};
// @HELPER_CLASS@
// @HELPER_CTOR@
json AIHelper::executeCurl(const json&) { ++modelCalls; return "answer"; }
void AIHelper::executeCurlStream(const json&, std::function<void(const std::string&)> token,
                                std::function<void()> done, std::function<void(const std::string&)>) {
    ++modelCalls; if (token) token("answer"); if (done) done();
}
// @HELPER_METHODS@
// @SNAPSHOT_FEATURE@

struct Row {
    long long userId;
    std::string sessionId;
    int isUser;
    std::string content;
    long long ts;
};
static std::vector<Row> rows;
static int nextErrorAt = -1, readErrorAt = -1, queryAttempts = 0;
static bool queryError = false, afterQueryError = false;
static std::string lastQuery;
namespace sql {
class ResultSet {
    int index_ = -1;
public:
    bool next() {
        ++index_;
        if (index_ == nextErrorAt) throw std::runtime_error("result traversal failed");
        return static_cast<size_t>(index_) < rows.size();
    }
    long long getInt64(const std::string& key) const { return key == "id" ? rows.at(index_).userId : rows.at(index_).ts; }
    int getInt(const std::string& key) const {
        if (key != "is_user") throw std::runtime_error("unexpected integer field");
        return rows.at(index_).isUser;
    }
    std::string getString(const std::string& key) const {
        if (index_ == readErrorAt && key == "content") throw std::runtime_error("row decode failed");
        if (key == "session_id") return rows.at(index_).sessionId;
        if (key == "username") return "stored username";
        return rows.at(index_).content;
    }
};
}
namespace muduo::net { class EventLoop {}; struct TcpServer { enum Option { kNoReusePort }; }; }
#define LOG_INFO std::cerr
namespace http {
class HttpResponse {
public:
    enum HttpStatusCode { k200Ok = 200, k400BadRequest = 400, k401Unauthorized = 401 };
    int status = 0;
    std::string body;
    void setStatusLine(const std::string&, HttpStatusCode code, const std::string&) { status = code; }
    void setCloseConnection(bool) {}
    void setContentType(const std::string&) {}
    void setContentLength(size_t) {}
    void setBody(const std::string& value) { body = value; }
};
struct HttpRequest {
    int userId = 1;
    bool authenticated = true;
    std::string body;
    std::string getVersion() const { return "HTTP/1.1"; }
    const std::string& getBody() const { return body; }
};
namespace session {
struct Session {
    int userId;
    bool authenticated;
    std::string getValue(const std::string& key) const {
        return key == "userId" ? std::to_string(userId) : key == "isLoggedIn" ? (authenticated ? "true" : "false") : "name";
    }
};
struct SessionManager {
    std::shared_ptr<Session> getSession(const HttpRequest& req, HttpResponse*) { return std::make_shared<Session>(Session{req.userId, req.authenticated}); }
};
}
struct HttpServer {
    session::SessionManager manager;
    session::SessionManager* getSessionManager() { return &manager; }
    session::SessionManager* getSessionManager() const { return const_cast<session::SessionManager*>(&manager); }
    void setSessionManager(std::unique_ptr<session::SessionManager>) {}
    muduo::net::EventLoop* getLoop() const { return nullptr; }
};
struct MysqlUtil {
    template<class Consumer> void consumeQuery(const std::string& query, Consumer consume) {
        ++queryAttempts; lastQuery = query;
        if (queryError) throw std::runtime_error("query failed");
        sql::ResultSet result;
        consume(result);
        if (afterQueryError) throw std::runtime_error("query completion failed");
    }
};
}
struct ImageRecognizer {};
class ChatLogoutHandler;
#define private public
// @SERVER_CLASS@
#undef private
ChatServer::ChatServer(int, const std::string&, muduo::net::TcpServer::Option) {}
void ChatServer::packageResp(const std::string&, http::HttpResponse::HttpStatusCode code, const std::string&, bool,
                             const std::string&, int, const std::string& body, http::HttpResponse* response) {
    response->status = code; response->body = body;
}
// @SERVER_METHODS@
class ChatHistoryHandler {
public:
    ChatServer* server_;
    void handle(const http::HttpRequest&, http::HttpResponse*);
};
// @HISTORY_HANDLER@

void require(bool valid, const char* message) { if (!valid) throw std::runtime_error(message); }
template<class F> void expectThrow(F action) {
    bool threw = false;
    try { action(); } catch (const std::exception&) { threw = true; }
    require(threw, "operation incorrectly reported success");
}
http::HttpResponse historyResponse(ChatServer& server, int userId, const std::string& session) {
    ChatHistoryHandler handler{&server};
    http::HttpRequest request; request.userId = userId; request.body = json{{"sessionId", session}}.dump();
    http::HttpResponse response; handler.handle(request, &response);
    return response;
}
json history(ChatServer& server, int userId, const std::string& session) {
    const auto response = historyResponse(server, userId, session);
    require(response.status == 200, "history endpoint failed");
    return json::parse(response.body).at("history");
}
void prior(ChatServer& server) {
    rows = {{7, "old", 1, "old question", 10}, {7, "old", 0, "old answer", 11}};
    server.initChatMessage();
}
int main(int argc, char* argv[]) {
    if (argc == 2) {
        std::ifstream persisted(argv[1]);
        const auto saved = json::parse(persisted);
        for (const auto& item : saved.at("rows"))
            rows.push_back({item[0].get<long long>(), item[1].get<std::string>(), item[2].get<int>(),
                            item[3].get<std::string>(), item[4].get<long long>()});
        ChatServer server(80, "restarted-fixture");
        server.initChatMessage();
        require(history(server, 42, "restart") == saved.at("expected"), "fresh process did not restore exact disk-fixture metadata");
        const auto beforeLookup = server.chatInformation;
        require(historyResponse(server, 99, "restart").status == 400 && server.chatInformation == beforeLookup, "fresh process crossed user ownership or created an unknown session");
        server.chatInformation.at(42).at("restart")->chat(42, "name", "restart", "new question", "5");
        require(modelPayloads == std::vector<Messages>{{{"persistent question", 1000}, {"persistent answer", 1001}, {"new question", 0}}}, "fresh process did not replay complete local context");
        require(localKeys == std::vector<std::string>{"42:restart"}, "fresh process used wrong model session identity");
        std::cout << "RESTART_REPLAY_OK\n";
        return 0;
    }
    int failed = 0, cases = 0;
    auto test = [&](const std::string& name, auto run) {
        ++cases; rows.clear(); nextErrorAt = readErrorAt = -1; queryError = afterQueryError = false;
        modelCalls = publishCalls = queryAttempts = 0; queueAccept = true; modelPayloads.clear(); localKeys.clear();
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    };
    test("exact_content_timestamps_and_user_session_ownership", [&] {
        ChatServer server(80, "test");
        const std::string binary("question\0tail", 13);
        rows = {{1, "shared", 1, binary, 100}, {2, "shared", 1, "other user", 100},
                {1, "second", 1, "另一会话", 101}, {1, "shared", 0, "answer'\\", 102},
                {2, "shared", 0, "other answer", 103}, {1, "second", 0, "回复", 104}};
        server.initChatMessage();
        require(server.chatInformation.size() == 2 && server.chatInformation.at(1).size() == 2, "ownership reconstruction failed");
        require(server.chatInformation.at(1).at("shared")->GetMessages() == Messages{{binary, 100}, {"answer'\\", 102}}, "stored bytes/timestamps changed");
        require(server.chatInformation.at(2).at("shared")->GetMessages() == Messages{{"other user", 100}, {"other answer", 103}}, "same session id crossed user boundary");
        require(server.sessionsIdsMap.at(1) == std::vector<std::string>{"shared", "second"}, "session list changed order or duplicated ids");
        require(publishCalls == 0 && modelCalls == 0, "loading history triggered external work");
    });
    test("consecutive_loads_replace_without_duplicating_history_or_sessions", [&] {
        ChatServer server(80, "test"); prior(server); auto expected = server.chatInformation.at(7).at("old")->GetMessages();
        server.initChatMessage();
        require(server.chatInformation.at(7).at("old")->GetMessages() == expected, "second load duplicated stored messages");
        require(server.sessionsIdsMap.at(7) == std::vector<std::string>{"old"}, "second load duplicated session id");
    });
    for (const std::string failure : {"query", "next", "read", "completion"}) {
        test("failed_" + failure + "_leaves_all_prior_maps_and_metadata_unchanged", [&] {
            ChatServer server(80, "test"); prior(server);
            const auto before = server.chatInformation;
            const auto beforeIds = server.sessionsIdsMap;
            rows = {{9, "new", 1, "staged question", 100}, {9, "new", 0, "staged answer", 101}};
            queryError = failure == "query"; nextErrorAt = failure == "next" ? 1 : -1;
            readErrorAt = failure == "read" ? 1 : -1; afterQueryError = failure == "completion";
            expectThrow([&] { server.initChatMessage(); });
            require(server.chatInformation == before && server.sessionsIdsMap == beforeIds, "failed restore published partial state");
            require(before.at(7).at("old")->GetMessages() == Messages{{"old question", 10}, {"old answer", 11}}, "old helper changed on failure");
            queryError = afterQueryError = false; nextErrorAt = readErrorAt = -1;
            server.initChatMessage(); server.initChatMessage();
            require(server.chatInformation.size() == 1 && server.chatInformation.at(9).at("new")->GetMessages().size() == 2, "retry retained old state or duplicated messages");
            require(server.sessionsIdsMap.at(9) == std::vector<std::string>{"new"}, "retry duplicated session metadata");
        });
    }
    test("successful_empty_result_replaces_previous_snapshot", [&] {
        ChatServer server(80, "test"); prior(server); rows.clear(); server.initChatMessage();
        require(server.chatInformation.empty() && server.sessionsIdsMap.empty(), "empty DB snapshot left stale in-memory history");
    });
    for (long long invalidId : {0LL, -1LL, static_cast<long long>(INT_MAX) + 1}) {
        test("invalid_user_id_does_not_overflow_or_publish_" + std::to_string(invalidId), [&] {
            ChatServer server(80, "test"); prior(server); const auto before = server.chatInformation;
            rows = {{invalidId, "bad", 1, "must not attach to another user", 100}};
            expectThrow([&] { server.initChatMessage(); }); require(server.chatInformation == before, "invalid ownership published");
        });
    }
    test("invalid_role_value_aborts_snapshot_without_silent_coercion", [&] {
        ChatServer server(80, "test"); prior(server); const auto before = server.chatInformation;
        rows = {{1, "bad", 2, "invalid role", 100}};
        expectThrow([&] { server.initChatMessage(); }); require(server.chatInformation == before, "invalid role published");
    });
    test("history_read_rejects_unknown_empty_foreign_ids_without_creating_sessions", [&] {
        ChatServer server(80, "test"); prior(server);
        const auto before = server.chatInformation;
        const auto beforeIds = server.sessionsIdsMap;
        for (const auto& request : std::vector<std::pair<int, std::string>>{{7, "unknown"}, {7, ""}, {9, "old"}, {9, "unknown"}}) {
            require(historyResponse(server, request.first, request.second).status == 400, "unknown or foreign history read was accepted");
            require(server.chatInformation == before && server.sessionsIdsMap == beforeIds, "history read created a caller-supplied session");
        }
        require(history(server, 7, "old").size() == 2, "owned history read stopped working");
    });
    test("legacy_roles_remain_visible_with_exact_stored_timestamps", [&] {
        ChatServer server(80, "test"); rows = {{1, "legacy", 0, "assistant first", 111}, {1, "legacy", 1, "later user", 112}, {1, "legacy", 1, "another user", 113}};
        server.initChatMessage();
        require(history(server, 1, "legacy") == json::array({{{"is_user", false}, {"content", "assistant first"}, {"ts", 111}},
                {{"is_user", true}, {"content", "later user"}, {"ts", 112}}, {{"is_user", true}, {"content", "another user"}, {"ts", 113}}}), "legacy role/content/timestamp was dropped or fabricated");
    });
    test("legacy_session_blocks_sync_generation_without_harming_other_sessions", [&] {
        ChatServer server(80, "test"); rows = {{1, "legacy", 0, "assistant first", 100}, {1, "normal", 1, "q", 101}, {1, "normal", 0, "a", 102}};
        server.initChatMessage(); auto legacy = server.chatInformation.at(1).at("legacy"); const auto before = legacy->GetMessages();
        expectThrow([&] { legacy->chat(1, "name", "legacy", "new", "1"); });
        require(legacy->GetMessages() == before && modelCalls == 0 && publishCalls == 0, "legacy refusal modified history or contacted upstream");
        require(server.chatInformation.at(1).at("normal")->chat(1, "name", "normal", "new", "1") == "answer", "other session was blocked");
    });
    test("legacy_session_blocks_stream_with_one_error_and_unchanged_history", [&] {
        ChatServer server(80, "test"); rows = {{1, "legacy", 1, "q", 100}, {1, "legacy", 1, "adjacent user", 101}};
        server.initChatMessage(); auto helper = server.chatInformation.at(1).at("legacy"); const auto before = helper->GetMessages();
        int errors = 0, done = 0, chunks = 0;
        helper->chatStream(1, "name", "legacy", "new", "5", [&](const auto&) { ++chunks; }, [&](const auto&) { ++done; }, [&](const auto&) { ++errors; });
        require(errors == 1 && done == 0 && chunks == 0 && modelCalls == 0 && publishCalls == 0 && helper->GetMessages() == before, "legacy stream changed data or reported success");
    });
    test("normal_restored_history_replays_exactly_into_local_model", [&] {
        ChatServer server(80, "test"); rows = {{42, "local", 1, "old question", 100}, {42, "local", 0, "old answer", 101}};
        server.initChatMessage(); auto helper = server.chatInformation.at(42).at("local");
        helper->chat(42, "name", "local", "new question", "5");
        require(modelPayloads == std::vector<Messages>{{{"old question", 100}, {"old answer", 101}, {"new question", 0}}} && localKeys == std::vector<std::string>{"42:local"}, "local replay lost history, metadata or ownership");
        auto output = history(server, 42, "local"); require(output.size() == 4 && output[2]["is_user"] == true && output[3]["is_user"] == false, "committed exchange role metadata was not appended");
    });
    test("queue_refusal_keeps_message_and_role_snapshots_unchanged", [&] {
        ChatServer server(80, "test"); prior(server); auto helper = server.chatInformation.at(7).at("old"); const auto before = history(server, 7, "old");
        queueAccept = false; expectThrow([&] { helper->chat(7, "name", "old", "new", "1"); });
        require(history(server, 7, "old") == before, "queue failure changed role-aware snapshot");
    });
    test("history_snapshot_coherent_during_concurrent_legacy_api_restore", [&] {
#if HISTORY_HAS_SNAPSHOT
        AIHelper helper;
        std::thread writer([&] { for (int i = 0; i < 300; ++i) helper.restoreMessage(std::to_string(i), i); });
        bool coherent = true;
        for (int repeat = 0; repeat < 300; ++repeat) {
            const auto snapshot = helper.GetHistorySnapshot();
            for (size_t i = 0; i < snapshot.size(); ++i)
                coherent = coherent && snapshot[i].timestamp == static_cast<long long>(i) && snapshot[i].content == std::to_string(i) && snapshot[i].isUser == (i % 2 == 0);
        }
        writer.join(); require(coherent && helper.GetHistorySnapshot().size() == 300, "snapshot mixed content and role metadata");
#else
        require(false, "coherent role-aware snapshot API missing");
#endif
    });
    std::cout << cases - failed << '/' << cases << " history cases passed\n";
    return failed ? 1 : 0;
}
