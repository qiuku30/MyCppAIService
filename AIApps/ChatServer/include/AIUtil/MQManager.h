#pragma once
#include <amqpcpp.h>
#include <amqpcpp/linux_tcp/tcpparent.h>
#include <amqpcpp/linux_tcp/tcphandler.h>
#include <amqpcpp/linux_tcp/tcpconnection.h>
#include <amqpcpp/linux_tcp/tcpchannel.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/Channel.h>
#include <muduo/net/TimerId.h>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

class MQManager {
public:
    enum class Disposition { Ack, Retry, Reject };
    using AckFn = std::function<void(Disposition)>;
    using MessageHandler = std::function<void(const std::string&, uint64_t, AckFn)>;
    static MQManager& instance() { static MQManager manager; return manager; }
    ~MQManager();
    void start(muduo::net::EventLoop*, const std::string&, MessageHandler);
    // true means bounded in-process admission, not a broker/disk receipt.
    bool publish(const std::string& queue, const std::string& message);
    // Call on the owning EventLoop before destroying that loop.
    void shutdown();

private:
    MQManager() = default;
    class Handler : public AMQP::TcpHandler,
                    public std::enable_shared_from_this<Handler> {
    public:
        using Completion = std::function<void(bool)>;
        Handler(muduo::net::EventLoop*, const AMQP::Address&,
                const std::string&, MessageHandler);
        ~Handler();
        void connect();
        void disconnect();
        void stop();
        bool schedulePublish(std::string, std::string, Completion = {});
    private:
        struct Publication {
            std::string queue, body;
            Completion completion;
        };
        void monitor(AMQP::TcpConnection*, int, int) override;
        void onConnected(AMQP::TcpConnection*) override;
        void onLost(AMQP::TcpConnection*) override;
        void onError(AMQP::TcpConnection*, const char*) override;
        void retireFd(int);
        void connectionFailed();
        void setupProducer();
        void startConsumer();
        void onConsumerMessage(const AMQP::Message&, uint64_t, bool);
        void completeDelivery(uint64_t generation, uint64_t tag,
                              Disposition, std::string body);
        void scheduleReconnect();
        void drainPending();
        void confirm(uint64_t tag, bool multiple, bool accepted);
        muduo::net::EventLoop* loop_;
        AMQP::Address address_;
        std::string queueName_;
        MessageHandler messageHandler_;
        std::shared_ptr<std::atomic<bool>> alive_ =
            std::make_shared<std::atomic<bool>>(true);
        std::unique_ptr<AMQP::TcpConnection> connection_;
        std::unique_ptr<AMQP::TcpChannel> producerChannel_, consumerChannel_;
        std::map<int, std::shared_ptr<muduo::net::Channel>> channels_;
        std::deque<Publication> pending_;
        std::map<uint64_t, Publication> inflight_;
        std::atomic<size_t> outstanding_{0};
        uint64_t generation_ = 0, nextPublishTag_ = 1;
        bool producerReady_ = false, shuttingDown_ = false;
        bool reconnectScheduled_ = false, recovering_ = false;
        bool constructingConnection_ = false;
        int reconnectDelaySec_ = 5;
        muduo::net::TimerId reconnectTimerId_;
        static constexpr size_t MaxPending = 10000;
        static constexpr size_t MaxInflight = 128;
    };
    std::mutex mutex_;
    std::shared_ptr<Handler> handler_;
};
