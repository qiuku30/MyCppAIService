#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace AMQP {
constexpr int readable = 1, writable = 2, durable = 4, requeue = 8, mandatory = 16;
class Address { public: explicit Address(const std::string&) {} };
class TcpConnection;
class TcpHandler {
public:
    virtual ~TcpHandler() = default;
    virtual void monitor(TcpConnection*, int, int) {}
    virtual void onConnected(TcpConnection*) {}
    virtual void onLost(TcpConnection*) {}
    virtual void onError(TcpConnection*, const char*) {}
};
class TcpConnection {
public:
    inline static int live = 0, destroyedDuringProcess = 0;
    inline static std::function<void(TcpConnection*)> onConstruct;
    TcpHandler* h;
    int watchedFd = -1, processCalls = 0;
    bool processingNow = false;
    std::function<void(int, int)> processing;
    explicit TcpConnection(TcpHandler* handler, const Address&) : h(handler) {
        ++live;
        if (onConstruct) onConstruct(this);
    }
    ~TcpConnection() {
        if (processingNow) ++destroyedDuringProcess;
        if (watchedFd >= 0) h->monitor(this, watchedFd, 0);
        --live;
    }
    void process(int fd, int flags) {
        ++processCalls;
        processingNow = true;
        if (processing) processing(fd, flags);
        processingNow = false;
    }
};
class Envelope {
public:
    std::string data, messageId;
    int mode = 1;
    Envelope(const char* text, size_t length) : data(text, length) {}
    void setDeliveryMode(int value) { mode = value; }
    void setContentType(const std::string&) {}
    void setMessageID(const std::string& value) { messageId = value; }
};
class Message {
public:
    std::string data;
    explicit Message(std::string body) : data(std::move(body)) {}
    const char* body() const { return data.data(); }
    size_t bodySize() const { return data.size(); }
};
using SuccessCallback = std::function<void()>;
using ErrorCallback = std::function<void(const char*)>;
using AckCallback = std::function<void(uint64_t, bool)>;
using NackCallback = std::function<void(uint64_t, bool, bool)>;
using BounceCallback = std::function<void(const Message&, int16_t, const std::string&)>;
struct Deferred {
    SuccessCallback success;
    ErrorCallback error;
    Deferred& onSuccess(SuccessCallback callback) {
        success = std::move(callback);
        if (success) success();
        return *this;
    }
    Deferred& onError(ErrorCallback callback) { error = std::move(callback); return *this; }
};
struct DeferredQueue : Deferred {
    std::string name;
    DeferredQueue& onSuccess(std::function<void(const std::string&, uint32_t, uint32_t)> callback) {
        callback(name, 0, 0); return *this;
    }
};
struct DeferredConsumer : Deferred {
    std::function<void(const Message&, uint64_t, bool)> received;
    DeferredConsumer& onReceived(std::function<void(const Message&, uint64_t, bool)> callback) {
        received = std::move(callback); return *this;
    }
};
struct DeferredConfirm : Deferred {
    AckCallback ack;
    NackCallback nack;
    DeferredConfirm& onSuccess(SuccessCallback callback) {
        Deferred::onSuccess(std::move(callback)); return *this;
    }
    DeferredConfirm& onAck(AckCallback callback) { ack = std::move(callback); return *this; }
    DeferredConfirm& onNack(NackCallback callback) { nack = std::move(callback); return *this; }
};
struct DeferredRecall : Deferred {
    BounceCallback received;
    DeferredRecall& onReceived(BounceCallback callback) { received = std::move(callback); return *this; }
};
struct Published { std::string queue, body; int mode, flags; bool accepted; };
struct Delivery { uint64_t tag; int flags; };
class TcpChannel {
public:
    inline static std::vector<Published> published;
    inline static std::vector<Delivery> acknowledgments, rejections;
    inline static int acked = 0, rejected = 0, live = 0;
    inline static bool allowPublish = true, autoReady = true;
    std::deque<DeferredQueue> declarations;
    std::vector<std::pair<std::string, int>> declaredQueues;
    DeferredConsumer consumer;
    DeferredConfirm confirm;
    DeferredRecall returned;
    SuccessCallback ready;
    ErrorCallback error;
    explicit TcpChannel(TcpConnection*) { ++live; }
    ~TcpChannel() { --live; }
    void onReady(SuccessCallback callback) {
        ready = std::move(callback);
        if (autoReady && ready) ready();
    }
    void onError(ErrorCallback callback) { error = std::move(callback); }
    DeferredQueue& declareQueue(const std::string& name, int flags) {
        declaredQueues.emplace_back(name, flags);
        declarations.emplace_back();
        declarations.back().name = name;
        return declarations.back();
    }
    void setQos(int) {}
    DeferredConsumer& consume(const std::string&) { return consumer; }
    DeferredConfirm& confirmSelect() { return confirm; }
    DeferredRecall& recall() { return returned; }
    bool publish(const std::string&, const std::string& queue, const std::string& body) {
        published.push_back({queue, body, 1, 0, allowPublish}); return allowPublish;
    }
    bool publish(const std::string&, const std::string& queue, const Envelope& envelope, int flags = 0) {
        published.push_back({queue, envelope.data, envelope.mode, flags, allowPublish}); return allowPublish;
    }
    bool ack(uint64_t tag, int flags = 0) {
        ++acked; acknowledgments.push_back({tag, flags}); return true;
    }
    bool reject(uint64_t tag, int flags = 0) {
        ++rejected; rejections.push_back({tag, flags}); return true;
    }
};
}
