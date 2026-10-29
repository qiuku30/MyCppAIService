#include "auth_fixture.h"
#if __has_include("HttpServer/include/utils/PasswordUtil.h")
#include "HttpServer/include/utils/PasswordUtil.h"
#define HAS_PASSWORD_UTIL 1
#endif
static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
int main() {
    try {
        const std::string malicious = "x'); DROP TABLE users; --";
        const std::string password = "secret'\\\"password";
        ChatRegisterHandler registration;
        require(registration.insertUser(malicious, password) == 7, "registration succeeds");
        for (const auto& call : registration.mysqlUtil_.calls) {
            require(call.text.find(malicious) == std::string::npos, "registration username must be bound, never concatenated into SQL");
            require(call.text.find(password) == std::string::npos, "registration password must never be embedded into SQL");
            require(!call.parameters.empty(), "all registration statements bind their user inputs");
        }
        auto stored = registration.mysqlUtil_.storedPassword;
        require(stored != password && stored.find("pbkdf2_sha256$") == 0, "registration stores a salted password hash");
#ifdef HAS_PASSWORD_UTIL
        require(http::PasswordUtil::verify(password, stored), "stored password verifies");
        require(!http::PasswordUtil::verify("incorrect", stored), "wrong password rejected");
        auto other = http::PasswordUtil::hash(password);
        require(other != stored, "fresh salts produce distinct encodings");
        other.back() = other.back() == '0' ? '1' : '0';
        require(!http::PasswordUtil::verify(password, other), "tampered hash rejected");
        require(!http::PasswordUtil::verify(password, "pbkdf2_sha256$999999999$bad$bad"), "malformed cost rejected");
        // Independently generated with Python hashlib.pbkdf2_hmac('sha256', ...).
        const std::string known = "pbkdf2_sha256$600000$000102030405060708090a0b0c0d0e0f$382b1e572fba3ac547369e7a7e3085617f972bf48e413a8d900e080f4c66a3bd";
        require(http::PasswordUtil::verify("known-test-password", known), "PBKDF2 matches independent SHA256 vector");
        require(!http::PasswordUtil::verify(password, stored + "$extra"), "trailing fields rejected");
        const std::string embeddedNull("a\0b", 3);
        require(http::PasswordUtil::verify(embeddedNull, http::PasswordUtil::hash(embeddedNull)), "embedded NUL bytes are hashed without truncation");
        ChatLoginHandler login;
        login.mysqlUtil_.exists = true;
        login.mysqlUtil_.storedPassword = stored;
        require(login.queryUserId(malicious, password) == 7, "hashed account logs in");
        require(login.queryUserId(malicious, "incorrect") == -1, "wrong hashed login denied");
        login.mysqlUtil_.storedPassword = password;
        login.mysqlUtil_.calls.clear();
        require(login.queryUserId(malicious, "incorrect") == -1, "legacy wrong password denied");
        require(login.mysqlUtil_.storedPassword == password, "wrong legacy password cannot migrate");
        require(login.queryUserId(malicious, password) == 7, "legacy account logs in");
        require(login.mysqlUtil_.storedPassword.find("pbkdf2_sha256$") == 0, "successful legacy login upgrades password");
        require(http::PasswordUtil::verify(password, login.mysqlUtil_.storedPassword), "legacy replacement verifies");
        for (const auto& call : login.mysqlUtil_.calls) {
            require(call.text.find(malicious) == std::string::npos, "login username bound");
            require(call.text.find(password) == std::string::npos, "login plaintext never appears in SQL");
        }
        login.mysqlUtil_.storedPassword = password;
        login.mysqlUtil_.updatedRows = 0;
        require(login.queryUserId(malicious, password) == -1, "legacy migration compare-and-swap failure cannot report success");
        require(login.mysqlUtil_.storedPassword == password, "failed compare-and-swap does not rewrite credential");
        login.mysqlUtil_.updatedRows = 1;
        login.mysqlUtil_.failUpdate = true;
        bool failed = false;
        try { login.queryUserId(malicious, password); } catch (const std::runtime_error&) { failed = true; }
        require(failed && login.mysqlUtil_.storedPassword == password, "migration write failure propagates without claiming an upgrade");
        login.mysqlUtil_.failUpdate = false;
        login.mysqlUtil_.storedPassword = "pbkdf2_sha256$malformed";
        require(login.queryUserId(malicious, login.mysqlUtil_.storedPassword) == -1, "malformed hash never falls back to plaintext verification");
#endif
        std::cout << "PASS: parameterized authentication, salted passwords, and legacy upgrade" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << std::endl;
        return 1;
    }
}
