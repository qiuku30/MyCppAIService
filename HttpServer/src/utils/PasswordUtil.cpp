#include "../../include/utils/PasswordUtil.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <array>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
constexpr int iterations = 600000;
constexpr std::string_view prefix = "pbkdf2_sha256$";
using Salt = std::array<unsigned char, 16>;
using Key = std::array<unsigned char, 32>;

template<size_t N>
std::string hex(const std::array<unsigned char, N>& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(N * 2);
    for (auto byte : bytes) {
        output.push_back(digits[byte >> 4]);
        output.push_back(digits[byte & 15]);
    }
    return output;
}

template<size_t N>
bool unhex(std::string_view input, std::array<unsigned char, N>& bytes) {
    if (input.size() != N * 2) return false;
    auto digit = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i < N; ++i) {
        int high = digit(input[2 * i]), low = digit(input[2 * i + 1]);
        if (high < 0 || low < 0) return false;
        bytes[i] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}

bool derive(const std::string& password, const Salt& salt, int cost, Key& key) {
    if (password.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return false;
    return PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                            salt.data(), static_cast<int>(salt.size()), cost,
                            EVP_sha256(), static_cast<int>(key.size()), key.data()) == 1;
}
}

namespace http {
std::string PasswordUtil::hash(const std::string& password) {
    Salt salt{};
    Key key{};
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1 ||
        !derive(password, salt, iterations, key)) {
        throw std::runtime_error("Password hashing failed");
    }
    auto result = std::string(prefix) + std::to_string(iterations) + "$" + hex(salt) + "$" + hex(key);
    OPENSSL_cleanse(key.data(), key.size());
    return result;
}

bool PasswordUtil::isHash(const std::string& encoded) {
    return encoded.compare(0, prefix.size(), prefix) == 0;
}

bool PasswordUtil::verify(const std::string& password, const std::string& encoded) {
    if (!isHash(encoded)) return false;
    std::string_view rest(encoded.data() + prefix.size(), encoded.size() - prefix.size());
    auto separator = rest.find('$');
    if (separator == std::string_view::npos) return false;
    int cost = 0;
    auto parsed = std::from_chars(rest.data(), rest.data() + separator, cost);
    if (parsed.ec != std::errc{} || parsed.ptr != rest.data() + separator ||
        cost < 100000 || cost > 2000000) return false;
    rest.remove_prefix(separator + 1);
    separator = rest.find('$');
    if (separator == std::string_view::npos) return false;
    Salt salt{};
    Key expected{}, actual{};
    if (!unhex(rest.substr(0, separator), salt) || !unhex(rest.substr(separator + 1), expected)) return false;
    const bool valid = derive(password, salt, cost, actual) &&
                       CRYPTO_memcmp(expected.data(), actual.data(), actual.size()) == 0;
    OPENSSL_cleanse(actual.data(), actual.size());
    return valid;
}

bool PasswordUtil::verifyLegacy(const std::string& password, const std::string& stored) {
    return !isHash(stored) && password.size() == stored.size() &&
           CRYPTO_memcmp(password.data(), stored.data(), password.size()) == 0;
}
}
