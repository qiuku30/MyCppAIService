#include "AIUtil/AISpeechProcessor.h"
#include <iostream>
#include <sstream>
int main() {
    curlResponses = {"{\"access_token\":\"TOKEN_SECRET\"}", "malformed SPEECH_PARSE_SECRET", "{\"error\":\"SPEECH_RESPONSE_SECRET\"}", "{\"result\":[\"TRANSCRIPT_SECRET\"]}"};
    std::ostringstream captured;
    auto* previous = std::cout.rdbuf(captured.rdbuf());
    AISpeechProcessor speech("CLIENT_SECRET", "PASSWORD_SECRET");
    const auto malformed = speech.recognize("RAW_SPEECH_SECRET");
    const auto missing = speech.recognize("RAW_SPEECH_SECRET");
    const auto valid = speech.recognize("RAW_SPEECH_SECRET");
    std::cout.rdbuf(previous);
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool condition, const std::string& name) { ++checks; if (!condition) { ++failures; std::cerr << "FAIL: " << name << '\n'; } };
    check(malformed.empty() && missing.empty() && valid == "TRANSCRIPT_SECRET", "speech result behavior preserved");
    for (const auto* marker : {"TOKEN_SECRET", "SPEECH_PARSE_SECRET", "SPEECH_RESPONSE_SECRET", "TRANSCRIPT_SECRET", "CLIENT_SECRET", "PASSWORD_SECRET", "RAW_SPEECH_SECRET"})
        check(captured.str().find(marker) == std::string::npos, std::string("speech log excludes ") + marker);
    std::cout << captured.str() << "speech checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
