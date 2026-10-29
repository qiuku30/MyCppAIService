#pragma once
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <map>
#include <string>
#include <muduo/base/Timestamp.h>
namespace muduo { namespace net {
class Buffer {
public:
    const char* peek() const { return data_.data() + offset_; }
    size_t readableBytes() const { return data_.size() - offset_; }
    const char* findCRLF() const {
        const auto pos = data_.find("\r\n", offset_);
        return pos == std::string::npos ? nullptr : data_.data() + pos;
    }
    void retrieve(size_t size) { offset_ += size; }
    void retrieveUntil(const char* end) { offset_ = end - data_.data(); }
    void append(const std::string& text) { data_ += text; }
    void append(const char* text) { data_ += text; }
private:
    std::string data_;
    size_t offset_ = 0;
};
}}
