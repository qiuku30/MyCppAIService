#include <nlohmann/json.hpp>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
using json = nlohmann::json;

// Fake libcurl transport feeds bytes into the actual production write callback.
using CURLcode = int;
constexpr CURLcode CURLE_OK = 0, CURLE_WRITE_ERROR = 23, CURLE_RECV_ERROR = 56;
constexpr int CURLOPT_URL = 1, CURLOPT_HTTPHEADER = 2, CURLOPT_POSTFIELDS = 3,
              CURLOPT_WRITEFUNCTION = 4, CURLOPT_WRITEDATA = 5,
              CURLINFO_RESPONSE_CODE = 6;
using Write = size_t (*)(void*, size_t, size_t, void*);
struct CURL { Write write = nullptr; void* data = nullptr; };
struct curl_slist {};
static std::vector<std::string> feed;
static CURLcode transport_result = CURLE_OK;
static long http_status = 200;
CURL* curl_easy_init() { return new CURL; }
void curl_easy_cleanup(CURL* c) { delete c; }
curl_slist* curl_slist_append(curl_slist*, const char*) { static curl_slist list; return &list; }
void curl_slist_free_all(curl_slist*) {}
const char* curl_easy_strerror(CURLcode) { return "fake transport error"; }
template <class T> CURLcode curl_easy_setopt(CURL* c, int option, T value) {
    if constexpr (std::is_convertible_v<T, Write>) {
        if (option == CURLOPT_WRITEFUNCTION) c->write = value;
    } else if constexpr (std::is_pointer_v<T>) {
        if (option == CURLOPT_WRITEDATA)
            c->data = const_cast<void*>(static_cast<const void*>(value));
    }
    return CURLE_OK;
}
CURLcode curl_easy_getinfo(CURL*, int, long* status) { *status = http_status; return CURLE_OK; }
CURLcode curl_easy_perform(CURL* c) {
    for (auto bytes : feed) {
        if (c->write(bytes.data(), 1, bytes.size(), c->data) != bytes.size()) return CURLE_WRITE_ERROR;
    }
    return transport_result;
}
class AIStrategy {
public:
    std::string getApiKey() const { return "fixture-secret"; }
    std::string getApiUrl() const { return "https://fixture.invalid"; }
    bool parseStreamChunk(const std::string&, std::string&) const;
    std::vector<std::string> streamHeaders() const { return {}; }
    bool isStreamComplete(const json&) const { return false; }
};
// @PRODUCTION_CHUNK_PARSER@
class AIHelper {
    std::shared_ptr<AIStrategy> strategy = std::make_shared<AIStrategy>();
public:
    void executeCurlStream(const json&, std::function<void(const std::string&)>,
                           std::function<void()>, std::function<void(const std::string&)>);
};
// @PRODUCTION_STREAM@

int main() {
    int failures = 0;
    const std::string chunk = "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n";
    auto test = [&](const std::string& name, std::vector<std::string> bytes,
                    CURLcode code, long status, bool success) {
        feed = std::move(bytes); transport_result = code; http_status = status;
        AIHelper helper;
        int errors = 0, dones = 0;
        std::string output;
        helper.executeCurlStream(json::object(), [&](const auto& text) { output += text; },
                                 [&] { ++dones; }, [&](const auto&) { ++errors; });
        const bool passed = output == "partial" &&
                            errors == (success ? 0 : 1) && dones == (success ? 1 : 0);
        std::cout << (passed ? "PASS " : "FAIL ") << name << " chunks=" << output
                  << " done=" << dones << " error=" << errors << '\n';
        if (!passed) ++failures;
    };
    test("curl_done_fragmented", {chunk.substr(0, 8), chunk.substr(8), "data: [DO", "NE]\n\n"}, CURLE_OK, 200, true);
    test("curl_eof_without_done", {chunk}, CURLE_OK, 200, false);
    test("curl_upstream_json_error", {chunk, "data: {\"error\":{\"message\":\"quota exceeded\"}}\n\n"}, CURLE_OK, 200, false);
    test("curl_transport_failure", {chunk}, CURLE_RECV_ERROR, 200, false);
    test("curl_http_failure", {chunk, "data: [DONE]\n\n"}, CURLE_OK, 503, false);
    test("curl_duplicate_done", {chunk, "data: [DONE]\n\ndata: [DONE]\n\n"}, CURLE_OK, 200, true);
    return failures ? 1 : 0;
}
