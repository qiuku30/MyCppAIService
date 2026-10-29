#include "../include/AIUtil/MQManager.h"
#include "../../../../HttpServer/include/utils/JsonUtil.h"
#include <muduo/base/Logging.h>
#include <cstdlib>
#include <algorithm>
#include <stdexcept>

MQManager::Handler::Handler(muduo::net::EventLoop* loop,
    const AMQP::Address& address, const std::string& queue, MessageHandler handler)
    : loop_(loop), address_(address), queueName_(queue),
      messageHandler_(std::move(handler)) {}

MQManager::Handler::~Handler() {
    alive_->store(false);
    shuttingDown_ = true;
    while (!channels_.empty()) retireFd(channels_.begin()->first);
    consumerChannel_.reset();
    producerChannel_.reset();
    connection_.reset();
}

void MQManager::Handler::retireFd(int fd) {
    auto it = channels_.find(fd);
    if (it == channels_.end()) return;
    auto channel = it->second;
    channel->disableAll();
    channel->remove();
    channels_.erase(it);
    // monitor() may be called by AMQP from inside this Channel's callback.
    loop_->queueInLoop([channel]() {});
}

void MQManager::Handler::connect() {
    loop_->assertInLoopThread();
    recovering_ = false;
    constructingConnection_ = true;
    try {
        connection_ = std::make_unique<AMQP::TcpConnection>(this, address_);
    } catch (...) {
        constructingConnection_ = false;
        connectionFailed();
        return;
    }
    constructingConnection_ = false;
}

void MQManager::Handler::disconnect() {
    loop_->assertInLoopThread();
    ++generation_;
    producerReady_ = false;
    while (!channels_.empty()) retireFd(channels_.begin()->first);
    // Unconfirmed sends are ambiguous, so retain and resend their stable IDs.
    for (auto it = inflight_.rbegin(); it != inflight_.rend(); ++it)
        pending_.push_front(std::move(it->second));
    inflight_.clear();
    nextPublishTag_ = 1;
    auto consumer = std::shared_ptr<AMQP::TcpChannel>(std::move(consumerChannel_));
    auto producer = std::shared_ptr<AMQP::TcpChannel>(std::move(producerChannel_));
    auto connection = std::shared_ptr<AMQP::TcpConnection>(std::move(connection_));
    auto self = shared_from_this();
    loop_->queueInLoop([self, consumer, producer, connection]() mutable {
        consumer.reset(); producer.reset(); connection.reset();
    });
}

void MQManager::Handler::stop() {
    loop_->assertInLoopThread();
    shuttingDown_ = true;
    alive_->store(false);
    loop_->cancel(reconnectTimerId_);
    disconnect();
}

void MQManager::Handler::monitor(AMQP::TcpConnection* connection, int fd, int flags) {
    if (fd < 0 || shuttingDown_) return;
    if (!constructingConnection_ && connection != connection_.get()) return;
    if (!flags) { retireFd(fd); return; }
    auto it = channels_.find(fd);
    if (it == channels_.end()) {
        auto channel = std::make_shared<muduo::net::Channel>(loop_, fd);
        auto weak = weak_from_this();
        channel->setReadCallback([weak, fd](muduo::Timestamp) {
            if (auto self = weak.lock(); self && self->alive_->load() &&
                self->connection_ && !self->recovering_)
                self->connection_->process(fd, AMQP::readable);
        });
        channel->setWriteCallback([weak, fd]() {
            if (auto self = weak.lock(); self && self->alive_->load() &&
                self->connection_ && !self->recovering_) {
                self->connection_->process(fd, AMQP::writable);
                self->drainPending();
            }
        });
        channel->setCloseCallback([weak]() {
            if (auto self = weak.lock()) self->connectionFailed();
        });
        it = channels_.emplace(fd, std::move(channel)).first;
    }
    if (flags & AMQP::readable) it->second->enableReading();
    else it->second->disableReading();
    if (flags & AMQP::writable) it->second->enableWriting();
    else it->second->disableWriting();
}

void MQManager::Handler::connectionFailed() {
    if (shuttingDown_ || recovering_) return;
    recovering_ = true;
    producerReady_ = false;
    auto weak = weak_from_this();
    loop_->queueInLoop([weak]() {
        if (auto self = weak.lock(); self && self->alive_->load()) {
            self->disconnect();
            self->scheduleReconnect();
        }
    });
}

void MQManager::Handler::onConnected(AMQP::TcpConnection*) {
    reconnectDelaySec_ = 5;
    setupProducer();
    startConsumer();
}
void MQManager::Handler::onLost(AMQP::TcpConnection*) { connectionFailed(); }
void MQManager::Handler::onError(AMQP::TcpConnection*, const char*) { connectionFailed(); }

void MQManager::Handler::setupProducer() {
    producerChannel_ = std::make_unique<AMQP::TcpChannel>(connection_.get());
    producerChannel_->onError([this](const char*) { connectionFailed(); });
    producerChannel_->recall().onReceived(
        [this](const AMQP::Message&, int16_t, const std::string&) { connectionFailed(); });
    producerChannel_->onReady([this]() {
        // Declare every destination before allowing publishes (including quarantine).
        producerChannel_->declareQueue(queueName_, AMQP::durable)
            .onSuccess([this](const std::string&, uint32_t, uint32_t) {
                producerChannel_->declareQueue(queueName_ + ".dead", AMQP::durable)
                    .onSuccess([this](const std::string&, uint32_t, uint32_t) {
                        producerChannel_->confirmSelect()
                            .onSuccess([this]() { producerReady_ = true; drainPending(); })
                            .onAck([this](uint64_t tag, bool multiple) { confirm(tag, multiple, true); })
                            .onNack([this](uint64_t tag, bool multiple, bool /*requeue*/) { confirm(tag, multiple, false); });
                    })
                    .onError([this](const char*) { connectionFailed(); });
            })
            .onError([this](const char*) { connectionFailed(); });
    });
}

void MQManager::Handler::startConsumer() {
    consumerChannel_ = std::make_unique<AMQP::TcpChannel>(connection_.get());
    consumerChannel_->onError([this](const char*) { connectionFailed(); });
    consumerChannel_->onReady([this]() {
        consumerChannel_->declareQueue(queueName_, AMQP::durable)
            .onSuccess([this](const std::string&, uint32_t, uint32_t) {
                consumerChannel_->setQos(1);
                consumerChannel_->consume(queueName_)
                    .onReceived([this](const AMQP::Message& msg, uint64_t tag, bool again) {
                        onConsumerMessage(msg, tag, again);
                    })
                    .onError([this](const char*) { connectionFailed(); });
            })
            .onError([this](const char*) { connectionFailed(); });
    });
}

void MQManager::Handler::onConsumerMessage(const AMQP::Message& msg,
                                            uint64_t tag, bool) {
    auto body = std::string(msg.body(), msg.bodySize());
    auto weak = weak_from_this();
    auto once = std::make_shared<std::atomic<bool>>(false);
    auto generation = generation_;
    auto ack = [weak, once, generation, tag, body](Disposition result) {
        if (once->exchange(true)) return;
        if (auto self = weak.lock()) self->loop_->queueInLoop(
            [weak, generation, tag, body, result]() {
                if (auto owner = weak.lock(); owner && owner->alive_->load())
                    owner->completeDelivery(generation, tag, result, body);
            });
    };
    try {
        if (messageHandler_) messageHandler_(body, tag, ack);
        else ack(Disposition::Retry);
    } catch (...) { ack(Disposition::Retry); }
}

void MQManager::Handler::completeDelivery(uint64_t generation, uint64_t tag,
                                           Disposition result, std::string body) {
    if (generation != generation_ || !consumerChannel_ || recovering_) return;
    if (result == Disposition::Ack) { consumerChannel_->ack(tag); return; }
    std::string target = queueName_ + ".dead";
    double delay = 0.0;
    if (result == Disposition::Retry) {
        try {
            auto job = json::parse(body);
            int attempt = job.value("_attempt", 0);
            if (attempt >= 0 && attempt < 5) {
                job["_attempt"] = attempt + 1;
                body = job.dump();
                target = queueName_;
                delay = std::min(30.0, static_cast<double>(1 << attempt));
            }
        } catch (...) {} // Legacy/unparseable bodies are quarantined, never executed.
    }
    auto weak = weak_from_this();
    loop_->runAfter(delay, [weak, generation, tag, target, body]() {
        auto self = weak.lock();
        if (!self || !self->alive_->load() || generation != self->generation_) return;
        auto confirmed = [weak, generation, tag](bool ok) {
            if (auto owner = weak.lock(); owner && owner->alive_->load() &&
                generation == owner->generation_ && owner->consumerChannel_) {
                if (ok) owner->consumerChannel_->ack(tag);
                else owner->consumerChannel_->reject(tag, AMQP::requeue);
            }
        };
        // ACK the original only after RabbitMQ confirms its replacement/quarantine.
        if (!self->schedulePublish(target, body, confirmed)) confirmed(false);
    });
}

bool MQManager::Handler::schedulePublish(std::string queue, std::string body,
                                          Completion completion) {
    if (!alive_->load()) return false;
    auto count = outstanding_.load();
    do {
        if (count >= MaxPending) return false;
    } while (!outstanding_.compare_exchange_weak(count, count + 1));
    auto weak = weak_from_this();
    loop_->queueInLoop([weak, item = Publication{std::move(queue), std::move(body),
                                                std::move(completion)}]() mutable {
        if (auto self = weak.lock(); self && self->alive_->load()) {
            self->pending_.push_back(std::move(item));
            self->drainPending();
        }
    });
    return true;
}

void MQManager::Handler::drainPending() {
    if (!producerReady_ || recovering_ || !producerChannel_) return;
    while (!pending_.empty() && inflight_.size() < MaxInflight) {
        auto& item = pending_.front();
        AMQP::Envelope envelope(item.body.data(), item.body.size());
        envelope.setDeliveryMode(2);
        envelope.setContentType("application/json");
        if (!producerChannel_->publish("", item.queue, envelope, AMQP::mandatory)) {
            connectionFailed();
            return;
        }
        inflight_.emplace(nextPublishTag_++, std::move(item));
        pending_.pop_front();
    }
}

void MQManager::Handler::confirm(uint64_t tag, bool multiple, bool accepted) {
    if (recovering_ || shuttingDown_) return;
    if (!accepted) { connectionFailed(); return; }
    for (auto it = inflight_.begin(); it != inflight_.end();) {
        if ((!multiple && it->first != tag) || (multiple && it->first > tag)) {
            ++it; continue;
        }
        auto completion = std::move(it->second.completion);
        it = inflight_.erase(it);
        outstanding_.fetch_sub(1);
        if (completion) completion(true);
    }
    drainPending();
}

void MQManager::Handler::scheduleReconnect() {
    if (shuttingDown_ || reconnectScheduled_) return;
    reconnectScheduled_ = true;
    auto weak = weak_from_this();
    reconnectTimerId_ = loop_->runAfter(reconnectDelaySec_, [weak]() {
        if (auto self = weak.lock(); self && self->alive_->load()) {
            self->reconnectScheduled_ = false;
            self->reconnectDelaySec_ = std::min(60, self->reconnectDelaySec_ * 2);
            self->connect();
        }
    });
}

MQManager::~MQManager() = default;
void MQManager::start(muduo::net::EventLoop* loop, const std::string& queue,
                       MessageHandler handler) {
    loop->assertInLoopThread();
    std::lock_guard<std::mutex> lock(mutex_);
    if (handler_) throw std::logic_error("MQManager already started");
    auto env = [](const char* key, const char* fallback) {
        const char* value = std::getenv(key); return std::string(value ? value : fallback);
    };
    AMQP::Address address("amqp://" + env("RABBITMQ_USER", "guest") + ":" +
        env("RABBITMQ_PASS", "guest") + "@" + env("RABBITMQ_HOST", "localhost") +
        ":" + env("RABBITMQ_PORT", "5672") + "/");
    handler_ = std::make_shared<Handler>(loop, address, queue, std::move(handler));
    handler_->connect();
}
bool MQManager::publish(const std::string& queue, const std::string& body) {
    std::lock_guard<std::mutex> lock(mutex_);
    return handler_ && handler_->schedulePublish(queue, body);
}
void MQManager::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!handler_) return;
    handler_->stop();
    handler_.reset();
}
