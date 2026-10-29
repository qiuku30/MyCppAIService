#pragma once
#include <openssl/rand.h>
#include <stdexcept>
#include <string>

inline std::string secureId() {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof bytes) != 1)
        throw std::runtime_error("Secure ID generation failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(32);
    for (auto byte : bytes) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}
