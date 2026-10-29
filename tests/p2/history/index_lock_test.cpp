// @HANDLER_PREFIX@

int main() {
    try {
        Server server;
        http::HttpRequest request; request.user_id = 2;
        request.body = json{{"question", "q"}}.dump();
        http::HttpResponse response;
        ChatCreateAndSendHandler(&server).handle(request, &response);
        require(response.status == 200 && helper_calls == 1, "session creation did not hold the session-index mutex during mutation");
        require(server.chatInformation.at(2).size() == 1 && server.sessionsIdsMap.at(2).size() == 1,
                "session creation failed to publish both indexes");
        std::cout << "PASS create_session_holds_shared_index_mutex\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL create_session_holds_shared_index_mutex: " << error.what() << '\n';
        return 1;
    }
}
