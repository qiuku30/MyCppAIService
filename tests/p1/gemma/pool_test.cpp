#include "gemma/gemma.h"
#include "AIUtil/GemmaInferencePool.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template <class F> void rejectedBeforeGenerate(F call) {
    const int before = gcpp::generate_calls;
    bool rejected = false;
    try { call(); } catch (const std::exception&) { rejected = true; }
    require(rejected && before == gcpp::generate_calls, "invalid request reached Generate");
}
std::vector<int> replayTokens(const GemmaInferencePool::History& history) {
    std::vector<int> expected;
    for (size_t i = 0; i < history.size(); ++i) {
        if (i % 2 == 0) expected.push_back(i == 0 ? 2 : 3);
        expected.insert(expected.end(), history[i].first.begin(), history[i].first.end());
    }
    return expected;
}
int main(int argc, char** argv) {
    const std::string test = argc > 1 ? argv[1] : "";
    auto& pool = GemmaInferencePool::instance();
    const auto dir = std::filesystem::temp_directory_path() /
                     ("gemma-p1-" + test + "-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const auto model = dir / "model.sbs";
    std::ofstream(model) << "fake model fixture";
    try {
        if (test == "policy") {
            for (int invalid = 0; invalid < 3; ++invalid) {
                GemmaInferencePool::CachePolicy policy;
                if (invalid == 0) policy.max_sessions = 0;
                if (invalid == 1) policy.sequence_tokens = 33;
                if (invalid == 2) policy.idle_ttl = 0ms;
                bool rejected = false;
                try { pool.initialize(model.string(), "", policy); } catch (const std::exception&) { rejected = true; }
                require(rejected && gcpp::init_calls == 0, "invalid cache policy reached engine");
            }
        } else if (test == "files") {
            bool rejected = false;
            try { pool.initialize((dir / "absent.sbs").string()); } catch (const std::exception&) { rejected = true; }
            require(rejected && gcpp::init_calls == 0, "missing files reached engine initialization");
            std::ofstream(dir / "empty.sbs");
            try { pool.initialize((dir / "empty.sbs").string()); } catch (const std::exception&) {}
            try { pool.initialize(model.string(), (dir / "absent.spm").string()); } catch (const std::exception&) {}
            try { pool.initialize(dir.string()); } catch (const std::exception&) {}
            require(!pool.isLoaded() && gcpp::init_calls == 0, "invalid file prevalidation entered engine");
        } else {
            GemmaInferencePool::CachePolicy policy;
            if (test == "ttl") policy.idle_ttl = 25ms;
            if (test == "past-budget") policy.sequence_tokens = 64;
            if (test == "model-cap") gcpp::model_sequence_limit = 64;
            pool.initialize(model.string(), "", policy);
            if (test == "bounded") {
                pool.generateSync("a", "hello", 0.7f, 8);
                const int first = gcpp::observed_cache;
                pool.generateSync("a", "again", 0.7f, 8);
                require(gcpp::observed_cache == first && gcpp::observed_pos > 0, "cache hit lost continuation");
                for (int i = 0; i < 12; ++i) pool.generateSync(std::to_string(i), "x", 0.7f, 8);
                require(gcpp::live_caches <= 2 && gcpp::peak_caches <= 2, "cache retention exceeded default limit two");
                require(gcpp::observed_capacity == 1024, "default sequence cache is larger than budget");
            } else if (test == "budget") {
                bool rejected = false;
                try { pool.generateSync("a", std::string(9000, 'x'), 0.7f, 8); } catch (const std::exception&) { rejected = true; }
                require(rejected && gcpp::generate_calls == 0, "oversized prompt reached Generate");
                rejectedBeforeGenerate([&] { pool.generateSync("a", "x", 0.7f, std::numeric_limits<size_t>::max()); });
                rejectedBeforeGenerate([&] { pool.generateSync("a", "!encode-failure!", 0.7f, 8); });
                rejectedBeforeGenerate([&] { pool.generateSync("a", "x", std::numeric_limits<float>::infinity(), 8); });
                rejectedBeforeGenerate([&] { pool.generateSync("a", "x", 0.7f, 0); });
                rejectedBeforeGenerate([&] { pool.generateSync("", "x", 0.7f, 8); });
            } else if (test == "model-cap") {
                rejectedBeforeGenerate([&] { pool.generateSync("a", std::string(80, 'x'), 0.7f, 8); });
                require(gcpp::live_caches == 0, "rejected capped-model request retained cache");
            } else if (test == "past-budget") {
                pool.generateSync("a", std::string(20, 'x'), 0.7f, 8);
                rejectedBeforeGenerate([&] { pool.generateSync("a", std::string(40, 'y'), 0.7f, 8); });
            } else if (test == "lru") {
                pool.generateSync("a", "a", 0.7f, 8);
                const int a = gcpp::observed_cache;
                pool.generateSync("b", "b", 0.7f, 8);
                const int b = gcpp::observed_cache;
                pool.generateSync("a", "a", 0.7f, 8);
                pool.generateSync("c", "c", 0.7f, 8);
                pool.generateSync("a", "a", 0.7f, 8);
                require(a == gcpp::observed_cache, "LRU evicted most recently used session");
                pool.generateSync("b", "b", 0.7f, 8);
                require(b != gcpp::observed_cache && gcpp::observed_pos == 0, "LRU retained least recently used session");
            } else if (test == "history" || test == "ttl") {
                pool.generateSync("mixed", "legacy", 0.7f, 8);
                const int incremental_cache = gcpp::observed_cache;
                pool.generateSyncFromHistory("mixed", {{"canonical", 0}}, 0.7f, 8);
                require(incremental_cache != gcpp::observed_cache && gcpp::observed_pos == 0,
                        "canonical history reused untracked incremental context");
                GemmaInferencePool::History history{{"first", 1}};
                const auto answer = pool.generateSyncFromHistory("user-a:session", history, 0.7f, 8);
                const int first_cache = gcpp::observed_cache;
                history = {{"first", 999}, {answer, 2}, {"second", 3}};
                if (test == "ttl") std::this_thread::sleep_for(40ms);
                pool.generateSyncFromHistory("user-a:session", history, 0.7f, 8);
                if (test == "ttl") {
                    require(first_cache != gcpp::observed_cache && gcpp::observed_pos == 0,
                            "idle session did not expire");
                    require(gcpp::observed_tokens == replayTokens(history), "expiry lost canonical history");
                } else {
                    require(first_cache == gcpp::observed_cache && gcpp::observed_pos > 0,
                            "exact transcript prefix did not reuse KV");
                    require(gcpp::observed_tokens == std::vector<int>({3, 's', 'e', 'c', 'o', 'n', 'd'}),
                            "cache hit replayed old prompt into live KV");
                    pool.generateSyncFromHistory("user-b:session", history, 0.7f, 8);
                    require(first_cache != gcpp::observed_cache && gcpp::observed_pos == 0,
                            "distinct user key shared cache");
                    history[0].first = "corrected";
                    pool.generateSyncFromHistory("user-a:session", history, 0.7f, 8);
                    require(first_cache != gcpp::observed_cache && gcpp::observed_pos == 0 &&
                            gcpp::observed_tokens == replayTokens(history), "changed history did not replay");
                    pool.generateSync("other", "x", 0.7f, 8);
                    pool.generateSync("another", "y", 0.7f, 8);
                    pool.generateSyncFromHistory("user-a:session", history, 0.7f, 8);
                    require(gcpp::observed_pos == 0 && gcpp::observed_tokens == replayTokens(history),
                            "eviction lost prior conversation");
                    rejectedBeforeGenerate([&] { pool.generateSyncFromHistory("a", {}, 0.7f, 8); });
                    rejectedBeforeGenerate([&] { pool.generateSyncFromHistory("a", {{"u", 0}, {"a", 0}}, 0.7f, 8); });
                    rejectedBeforeGenerate([&] { pool.generateSyncFromHistory("a", {{std::string(1200, 'x'), 0}}, 0.7f, 8); });
                }
            } else if (test == "callback-failure") {
                bool rejected = false;
                try {
                    pool.generateFromHistory("a", {{"first", 0}}, [](const auto&) { throw std::runtime_error("disconnected"); }, 0.7f, 8);
                } catch (const std::exception&) { rejected = true; }
                require(rejected && gcpp::live_caches == 0, "partial failed generation retained cache");
                pool.generateSyncFromHistory("a", {{"first", 0}}, 0.7f, 8);
                require(gcpp::observed_pos == 0, "failed generation reused partial KV");
            } else if (test == "concurrent") {
                std::vector<std::thread> workers;
                for (int i = 0; i < 8; ++i) workers.emplace_back([&, i] {
                    for (int j = 0; j < 12; ++j) pool.generateSync(std::to_string(i), "x", 0.7f, 8);
                });
                for (auto& worker : workers) worker.join();
                require(gcpp::peak_caches <= 2 && gcpp::generate_calls == 96, "concurrent cache bound or inference failed");
            } else if (test == "clear" || test == "shutdown") {
                gcpp::block_generate = true;
                auto inference = std::async(std::launch::async, [&] { pool.generateSync("a", "hello", 0.7f, 8); });
                {
                    std::unique_lock<std::mutex> lock(gcpp::barrier_mutex);
                    require(gcpp::barrier.wait_for(lock, 2s, [] { return gcpp::entered_generate; }), "generation did not start");
                }
                std::promise<void> ready;
                auto mutation = std::async(std::launch::async, [&] {
                    ready.set_value();
                    if (test == "clear") pool.clearSession("a"); else pool.shutdown();
                });
                ready.get_future().wait();
                const bool waited = mutation.wait_for(40ms) == std::future_status::timeout;
                {
                    std::lock_guard<std::mutex> lock(gcpp::barrier_mutex);
                    gcpp::release_generate = true;
                }
                gcpp::barrier.notify_all();
                inference.get(); mutation.get();
                require(waited && !gcpp::destroyed_while_generating, "lifecycle mutation destroyed active cache");
            } else throw std::runtime_error("unknown test");
        }
        pool.shutdown();
        require(gcpp::live_caches == 0, "shutdown retained cache");
        std::filesystem::remove_all(dir);
        std::cout << "PASS " << test << '\n';
        return 0;
    } catch (const std::exception& e) {
        pool.shutdown();
        std::filesystem::remove_all(dir);
        std::cerr << "FAIL " << test << ": " << e.what() << '\n';
        return 1;
    }
}
