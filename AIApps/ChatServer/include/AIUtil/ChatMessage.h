#pragma once
#include "../../../../HttpServer/include/utils/JsonUtil.h"
#include <algorithm>
#include <climits>
#include <stdexcept>
#include <string>

// One exchange is a single queue item and a single atomic SQL statement.
struct ChatExchange {
    std::string message_id;
    int user_id;
    std::string username, session_id, question, answer;
    long long ts;

    std::string serialize() const {
        return json{{"v", 1}, {"message_id", message_id}, {"user_id", user_id},
            {"username", username}, {"session_id", session_id}, {"question", question},
            {"answer", answer}, {"ts", ts}}.dump();
    }
    static ChatExchange parse(const std::string& body) {
        try {
            auto j = json::parse(body);
            if (!j.at("v").is_number_integer() || j.at("v") != 1 || !j.at("user_id").is_number_integer() ||
                !j.at("ts").is_number_integer()) throw std::invalid_argument("Invalid message schema");
            auto uid = j.at("user_id").get<long long>();
            ChatExchange item{j.at("message_id").get<std::string>(), 0,
                j.at("username").get<std::string>(), j.at("session_id").get<std::string>(),
                j.at("question").get<std::string>(), j.at("answer").get<std::string>(),
                j.at("ts").get<long long>()};
            if (uid <= 0 || uid > INT_MAX || item.ts < 0 || item.ts >= LLONG_MAX - 1 ||
                item.message_id.size() != 32 || !std::all_of(item.message_id.begin(), item.message_id.end(),
                    [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
                item.username.empty() || item.username.size() > 128 || item.session_id.empty() ||
                item.session_id.size() > 64 || item.question.empty() || item.answer.empty() ||
                item.question.size() > 65535 || item.answer.size() > 65535)
                throw std::invalid_argument("Invalid message fields");
            item.user_id = static_cast<int>(uid);
            return item;
        } catch (const std::exception&) {
            throw std::invalid_argument("Unsupported or malformed chat message");
        }
    }
    template<class Database> void persist(Database& database) const {
        const std::string userMessageId = message_id + "/u";
        const std::string answerMessageId = message_id + "/a";
        // Duplicate delivery is safe. Legacy rows have NULL message_id.
        database.executeUpdate(
            "INSERT INTO chat_message (message_id,id,username,session_id,is_user,content,ts) "
            "VALUES (?,?,?,?,?,?,?),(?,?,?,?,?,?,?) "
            "ON DUPLICATE KEY UPDATE message_id=message_id",
            userMessageId, user_id, username, session_id, 1, question, ts,
            answerMessageId, user_id, username, session_id, 0, answer, ts + 1);
    }
};
