#pragma once
#include <algorithm>
#include <cassert>
#include <map>
#include <string>

namespace muduo { namespace net {
class Buffer {
public:
    struct StringPiece {
        std::string value;
        std::string as_string() const { return value; }
    };
    const char* peek() const { return data_.data() + offset_; }
    size_t readableBytes() const { return data_.size() - offset_; }
    const char* findCRLF() const {
        const auto pos = data_.find("\r\n", offset_);
        return pos == std::string::npos ? nullptr : data_.data() + pos;
    }
    void retrieve(size_t size) { assert(size <= readableBytes()); offset_ += size; }
    void retrieveUntil(const char* end) { offset_ = end - data_.data(); }
    void append(const std::string& text) { data_ += text; }
    void append(const char* text) { data_ += text; }
    StringPiece toStringPiece() const { return {std::string(peek(), readableBytes())}; }
    std::string retrieveAllAsString() {
        auto result = toStringPiece().as_string();
        offset_ = data_.size();
        return result;
    }
private:
    std::string data_;
    size_t offset_ = 0;
};
}}
