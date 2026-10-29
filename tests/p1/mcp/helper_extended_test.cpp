#include "AIApps/ChatServer/include/AIUtil/ChatMessage.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
using Messages = std::vector<std::pair<std::string, long long>>;
static std::vector<std::string> scripted, admitted, local_keys;
static std::vector<Messages> payloads;
static bool stream_error = false, tool_error = false, queue_accept = true;
static std::atomic<int> errors{0}, dones{0}, publish_attempts{0};
std::string secureId() { return std::string(32, 'a'); } // external entropy boundary
struct MQManager {
    static MQManager& instance() { static MQManager manager; return manager; }
    bool publish(const std::string& queue, const std::string& body) {
        if (queue != "sql_queue") throw std::runtime_error("wrong queue");
        ++publish_attempts;
        if (!queue_accept) return false;
        admitted.push_back(body);
        return true;
    }
};
struct AIStrategy {
    bool isMCPModel = false, local = false;
    bool isLocal() const { return local; }
    json buildRequest(const Messages& history) const { payloads.push_back(history); return "payload"; }
    json buildStreamRequest(const Messages& history) const { return buildRequest(history); }
    std::string parseResponse(const json& response) const { return response.get<std::string>(); }
};
struct StrategyFactory {
    static StrategyFactory& instance() { static StrategyFactory factory; return factory; }
    std::shared_ptr<AIStrategy> create(const std::string& code) {
        auto strategy = std::make_shared<AIStrategy>();
        strategy->isMCPModel = code == "4";
        strategy->local = code == "5";
        return strategy;
    }
};
struct AIToolCall { bool isToolCall; std::string toolName; json args; };
struct AIConfig {
    bool loadFromFile(const std::string&) { return true; }
    std::string buildPrompt(const std::string& question) { return "TEMP1<" + question + ">"; }
    AIToolCall parseAIResponse(const std::string& response) { return {response == "CALL_TOOL", "tool", json::object()}; }
    std::string buildToolResultPrompt(const std::string& question, const std::string&, const json&, const json&) {
        return "TEMP2<" + question + ">";
    }
};
struct AIToolRegistry {
    json invoke(const std::string&, const json&) {
        if (tool_error) throw std::runtime_error("tool failed");
        return "result";
    }
};
struct GemmaInferencePool {
    static GemmaInferencePool& instance() { static GemmaInferencePool pool; return pool; }
    bool isLoaded() const { return true; }
    std::string generateSyncFromHistory(const std::string& key, const Messages& history) {
        local_keys.push_back(key); payloads.push_back(history); return "local answer";
    }
    void generateFromHistory(const std::string& key, const Messages& history,
                             std::function<void(const std::string&)> token) {
        token(generateSyncFromHistory(key, history));
    }
};
class AIHelper {
public:
    AIHelper() = default;
    void setStrategy(std::shared_ptr<AIStrategy>);
    void restoreMessage(const std::string&, long long);
    void restoreMessage(const std::string&, long long, bool);
    struct HistoryMessage { std::string content; long long timestamp; bool isUser; };
    std::vector<HistoryMessage> GetHistorySnapshot();
    std::string chat(int, std::string, std::string, std::string, std::string);
    void chatStream(int, std::string, std::string, std::string, std::string,
                    std::function<void(const std::string&)>, std::function<void(const std::string&)>,
                    std::function<void(const std::string&)>);
    Messages GetMessages();
    json request(const json&);
private:
    std::shared_ptr<AIStrategy> strategy;
    Messages messages;
    std::vector<bool> messageRoles_;
    void validateCanonicalHistory() const;
    std::mutex chat_mutex_;
    void commitExchange(int, const std::string&, const std::string&, const std::string&, const std::string&);
    json executeCurl(const json&) {
        if (!strategy) throw std::runtime_error("cloud strategy was not initialized");
        if (scripted.empty()) throw std::runtime_error("fixture exhausted");
        const auto response = scripted.front(); scripted.erase(scripted.begin());
        if (response == "THROW") throw std::runtime_error("upstream failed");
        return response;
    }
    void executeCurlStream(const json&, std::function<void(const std::string&)> token,
                           std::function<void()> done, std::function<void(const std::string&)> error) {
        if (stream_error) { if (error) error("stream failed"); return; }
        if (token) token("answer");
        if (done) done();
    }
};
// @PRODUCTION_METHODS@

void require(bool valid, const char* message) { if (!valid) throw std::runtime_error(message); }
template <class F> void expectThrow(F run) {
    bool threw = false; try { run(); } catch (const std::exception&) { threw = true; }
    require(threw, "expected failure");
}
void prior(AIHelper& helper) { helper.restoreMessage("prior question", 100); helper.restoreMessage("prior answer", 101); }
void reset() {
    scripted.clear(); admitted.clear(); payloads.clear(); local_keys.clear();
    stream_error = false; tool_error = false; queue_accept = true;
    errors = 0; dones = 0; publish_attempts = 0;
}
int main() {
    int failed = 0, cases = 0;
    auto test = [&](const std::string& name, auto run) {
        ++cases; reset();
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    };
    test("default_custom_request_initializes_strategy", [&] {
        AIHelper helper; scripted = {"answer"};
        require(helper.request(json::object()) == "answer", "custom request returned wrong response");
        require(helper.GetMessages().empty() && admitted.empty(), "custom request unexpectedly committed history");
    });
    test("mcp_no_tool_canonical_pair", [&] {
        AIHelper helper; scripted = {"answer"};
        helper.chat(1, "user", "sid", "question", "4");
        const auto history = helper.GetMessages();
        require(history.size() == 2 && history[0].first == "question" && history[1].first == "answer" && admitted.size() == 1,
                "MCP prompt leaked into canonical history");
    });
    test("mcp_stream_emits_content_and_done", [&] {
        AIHelper helper; scripted = {"answer"}; std::string chunks;
        helper.chatStream(1, "user", "sid", "question", "4", [&](const auto& text) { chunks += text; },
                          [&](const auto&) { ++dones; }, [&](const auto&) { ++errors; });
        require(chunks == "answer" && dones == 1 && errors == 0 && admitted.size() == 1,
                "MCP SSE omitted answer or completion");
    });
    test("cloud_stream_error_no_commit", [&] {
        AIHelper helper; prior(helper); const auto before = helper.GetMessages(); stream_error = true;
        helper.chatStream(1, "user", "sid", "question", "1", [](const auto&) {},
                          [&](const auto&) { ++dones; }, [&](const auto&) { ++errors; });
        require(errors == 1 && dones == 0 && admitted.empty() && helper.GetMessages() == before,
                "cloud error committed or completed");
    });
    test("cloud_retry_drops_failed_question", [&] {
        AIHelper helper; scripted = {"THROW"};
        expectThrow([&] { helper.chat(1, "user", "sid", "failed", "1"); });
        scripted = {"answer"}; helper.chat(1, "user", "sid", "retry", "1");
        require(payloads.back().size() == 1 && payloads.back()[0].first == "retry", "failed request leaked into retry");
    });
    test("empty_answer_not_committed", [&] {
        AIHelper helper; scripted = {""};
        expectThrow([&] { helper.chat(1, "user", "sid", "question", "1"); });
        require(admitted.empty() && helper.GetMessages().empty(), "empty answer committed");
    });
    for (bool tool_failure : {true, false}) test(tool_failure ? "tool_exception_preserves_prior" : "phase2_failure_preserves_prior", [&] {
        AIHelper helper; prior(helper); const auto before = helper.GetMessages();
        scripted = {"CALL_TOOL", "THROW"}; tool_error = tool_failure;
        helper.chatStream(1, "user", "sid", "question", "4", [](const auto&) {},
                          [&](const auto&) { ++dones; }, [&](const auto&) { ++errors; });
        require(errors == 1 && dones == 0, "MCP error completed successfully");
        require(helper.GetMessages() == before && admitted.empty(), "failed MCP changed canonical history");
    });
    test("mcp_success_prior_and_atomic_pair", [&] {
        AIHelper helper; prior(helper); const auto before = helper.GetMessages();
        scripted = {"CALL_TOOL", "answer"}; helper.chat(1, "user", "sid", "question", "4");
        const auto history = helper.GetMessages();
        require(history.size() == 4 && history[0] == before[0] && history[1] == before[1] &&
                history[2].first == "question" && history[3].first == "answer", "prior or final history changed");
        require(payloads.size() == 2 && payloads[0].size() == 3 && payloads[1].size() == 3 &&
                payloads[0][0] == before[0] && payloads[1][1] == before[1], "temporary context changed prior turns");
        require(admitted.size() == 1, "pair was not one queue item");
        const auto pair = ChatExchange::parse(admitted[0]);
        require(pair.question == "question" && pair.answer == "answer" && pair.user_id == 1 &&
                pair.session_id == "sid" && pair.ts == history[2].second && pair.ts + 1 == history[3].second,
                "queue body is not canonical atomic pair");
    });
    test("queue_rejection_memory_unchanged", [&] {
        AIHelper helper; prior(helper); const auto before = helper.GetMessages();
        scripted = {"answer"}; queue_accept = false;
        expectThrow([&] { helper.chat(1, "user", "sid", "question", "1"); });
        require(helper.GetMessages() == before && admitted.empty() && publish_attempts == 1,
                "queue rejection left accepted or partial history");
    });
    test("queue_rejection_stream_no_done", [&] {
        AIHelper helper; queue_accept = false;
        helper.chatStream(1, "user", "sid", "question", "1", [](const auto&) {},
                          [&](const auto&) { ++dones; }, [&](const auto&) { ++errors; });
        require(errors == 1 && dones == 0 && helper.GetMessages().empty() && admitted.empty(),
                "rejected stream exchange reported success");
    });
    test("local_user_session_namespace_and_full_history", [&] {
        AIHelper a, b; prior(a);
        a.chat(1, "a", "23", "question", "5"); b.chat(12, "b", "3", "question", "5");
        require(local_keys == std::vector<std::string>({"1:23", "12:3"}), "local key namespace collision");
        require(payloads[0].size() == 3 && payloads[0][0].first == "prior question", "local sync omitted prior history");
        a.chatStream(1, "a", "23", "again", "5", [](const auto&) {},
                     [&](const auto&) { ++dones; }, [&](const auto&) { ++errors; });
        require(local_keys.back() == "1:23" && payloads.back().size() == 5 && errors == 0 && dones == 1,
                "local stream key/full-history mismatch");
    });
    test("concurrent_exchange_pairs", [&] {
        AIHelper helper; scripted.assign(12, "answer");
        std::vector<std::thread> workers;
        for (int i = 0; i < 12; ++i) workers.emplace_back([&, i] { helper.chat(1, "user", "sid", "q" + std::to_string(i), "1"); });
        for (auto& worker : workers) worker.join();
        const auto history = helper.GetMessages();
        require(history.size() == 24 && admitted.size() == 12 && payloads.size() == 12, "concurrent requests lost pairs");
        for (size_t i = 0; i < history.size(); i += 2) {
            require(history[i].first[0] == 'q' && history[i + 1].first == "answer" &&
                    payloads[i / 2].size() == i + 1 && (i == 0 || history[i].second > history[i - 1].second),
                    "concurrent history interleaved or saw partial pair");
        }
    });
    test("restored_timestamp_overflow_rejected", [&] {
        AIHelper helper; helper.restoreMessage("prior question", LLONG_MAX - 1); helper.restoreMessage("prior answer", LLONG_MAX);
        const auto before = helper.GetMessages(); scripted = {"answer"};
        expectThrow([&] { helper.chat(1, "user", "sid", "question", "1"); });
        require(helper.GetMessages() == before && admitted.empty(), "overflow history was committed");
    });
    std::cout << "EXTENDED_HELPER_FAILURES=" << failed << '/' << cases << '\n';
    return failed ? 1 : 0;
}
