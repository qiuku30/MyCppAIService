#pragma once
#include <set>
#include <muduo/base/noncopyable.h>

namespace muduo { namespace net {
class EventLoop : muduo::noncopyable {
public:
    inline static std::set<const EventLoop*> liveLoops;
    EventLoop() { liveLoops.insert(this); }
    ~EventLoop() { liveLoops.erase(this); }
    void loop() {}
};
}}
