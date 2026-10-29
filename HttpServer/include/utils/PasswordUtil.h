#pragma once
#include <string>

namespace http {
class PasswordUtil {
public:
    static std::string hash(const std::string& password);
    static bool verify(const std::string& password, const std::string& encoded);
    static bool isHash(const std::string& encoded);
    // Temporary migration path for existing rows; never use for new credentials.
    static bool verifyLegacy(const std::string& password, const std::string& stored);
};
}
