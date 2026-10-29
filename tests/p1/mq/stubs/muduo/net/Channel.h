#pragma once
#include "EventLoop.h"
#include <map>

namespace muduo { namespace net {
class Channel {
public:
    inline static std::map<int, Channel*> active;
    inline static int badDestruction = 0, live = 0;
    int fd;
    bool added = false, handling = false, reading = false, writing = false;
    std::function<void(Timestamp)> read;
    std::function<void()> write, close;
    Channel(EventLoop*, int descriptor) : fd(descriptor) { ++live; }
    ~Channel() {
        if (added || handling) ++badDestruction;
        const auto it = active.find(fd);
        if (it != active.end() && it->second == this) active.erase(it);
        --live;
    }
    void setReadCallback(std::function<void(Timestamp)> callback) { read = std::move(callback); }
    void setWriteCallback(std::function<void()> callback) { write = std::move(callback); }
    void setCloseCallback(std::function<void()> callback) { close = std::move(callback); }
    void enableReading() { reading = true; added = true; active[fd] = this; }
    void enableWriting() { writing = true; added = true; active[fd] = this; }
    void disableReading() { reading = false; }
    void disableWriting() { writing = false; }
    void disableAll() { reading = false; writing = false; }
    void remove() { added = false; active.erase(fd); }
    void fireRead() { handling = true; if (read) read({}); handling = false; }
    void fireWrite() { handling = true; if (write) write(); handling = false; }
    void fireClose() { handling = true; if (close) close(); handling = false; }
};
}}
