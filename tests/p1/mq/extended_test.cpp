#include <atomic>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>
#include <amqpcpp.h>
#include <muduo/net/Channel.h>
#define private public
#include "AIApps/ChatServer/include/AIUtil/MQManager.h"
#undef private
#include <nlohmann/json.hpp>

namespace {
using Handler = MQManager::Handler;
using Disposition = MQManager::Disposition;
int failures = 0, checks = 0;
void check(bool ok, const char* message) {
    ++checks;
    std::cout << (ok ? "PASS " : "FAIL ") << message << '\n';
    failures += !ok;
}
struct Fixture {
    muduo::net::EventLoop loop;
    std::shared_ptr<Handler> handler;
    explicit Fixture(MQManager::MessageHandler callback = nullptr) {
        AMQP::TcpChannel::published.clear();
        AMQP::TcpChannel::acknowledgments.clear();
        AMQP::TcpChannel::rejections.clear();
        AMQP::TcpChannel::acked = AMQP::TcpChannel::rejected = 0;
        AMQP::TcpChannel::allowPublish = AMQP::TcpChannel::autoReady = true;
        AMQP::TcpConnection::onConstruct = {};
        handler = std::make_shared<Handler>(&loop, AMQP::Address("mock"), "sql_queue", std::move(callback));
    }
    void connect(bool ready = true) {
        handler->connect();
        if (ready) handler->onConnected(handler->connection_.get());
    }
    void reconnect() {
        loop.flush(); loop.tick();
        handler->onConnected(handler->connection_.get());
        loop.flush();
    }
    ~Fixture() {
        AMQP::TcpConnection::onConstruct = {};
        handler->stop(); loop.flush(); handler.reset(); loop.flush();
        check(muduo::net::Channel::active.empty(), "fixture removes every fd from the poller");
        check(muduo::net::Channel::live == 0 && AMQP::TcpConnection::live == 0 && AMQP::TcpChannel::live == 0,
              "fixture releases deferred Channels and AMQP resources");
    }
};

void callbackLifetime() {
    Fixture f;
    AMQP::TcpConnection::onConstruct = [](AMQP::TcpConnection* connection) {
        connection->watchedFd = 10;
        connection->h->monitor(connection, 10, AMQP::readable);
    };
    f.connect();
    check(f.handler->channels_.count(10) == 1, "constructor-time resolver monitor is registered");
    f.handler->monitor(f.handler->connection_.get(), 11, AMQP::writable);
    auto* oldChannel = f.handler->channels_.at(10).get();
    auto* connection = f.handler->connection_.get();
    connection->processing = [&](int fd, int) { f.handler->monitor(connection, fd, 0); };
    oldChannel->fireRead();
    check(f.handler->channels_.count(10) == 0 && f.handler->channels_.count(11) == 1,
          "unmonitor within read callback preserves the independent socket fd");
    check(muduo::net::Channel::live == 2 && muduo::net::Channel::badDestruction == 0,
          "unmonitored Channel lives until its callback returns");
    f.loop.flush();
    check(muduo::net::Channel::live == 1, "unmonitored Channel is released on a later loop turn");

    auto* socketChannel = f.handler->channels_.at(11).get();
    connection->processing = [&](int, int) { f.handler->onError(connection, "injected read failure"); };
    socketChannel->fireWrite();
    check(f.handler->recovering_ && f.handler->connection_.get() == connection,
          "AMQP process error defers disconnect until the callback returns");
    f.loop.flush();
    check(AMQP::TcpConnection::destroyedDuringProcess == 0 && muduo::net::Channel::badDestruction == 0,
          "read/write error recovery never destroys an active callback object");
}

void confirmsAndWindow() {
    Fixture f; f.connect();
    std::vector<int> completed;
    for (int i = 0; i < 3; ++i) f.handler->schedulePublish("sql_queue", std::to_string(i),
        [&, i](bool ok) { if (ok) completed.push_back(i); });
    f.loop.flush();
    check(f.handler->inflight_.size() == 3 && f.handler->outstanding_ == 3 && completed.empty(),
          "local publish retains all unconfirmed messages and completions");
    f.handler->producerChannel_->confirm.ack(2, false);
    check(f.handler->inflight_.size() == 2 && f.handler->outstanding_ == 2 && completed == std::vector<int>{1},
          "out-of-order single confirm releases only its own publication");
    f.handler->producerChannel_->confirm.ack(2, false);
    check(f.handler->outstanding_ == 2 && completed.size() == 1, "duplicate confirm cannot underflow admission count");
    f.handler->producerChannel_->confirm.ack(3, true);
    check(f.handler->inflight_.empty() && f.handler->outstanding_ == 0 && completed.size() == 3,
          "multiple confirm retires every earlier remaining tag");

    for (size_t i = 0; i < Handler::MaxInflight + 1; ++i)
        f.handler->schedulePublish("sql_queue", "window");
    f.loop.flush();
    check(f.handler->inflight_.size() == Handler::MaxInflight && f.handler->pending_.size() == 1,
          "publisher never exceeds the configured in-flight window");
    f.handler->producerChannel_->confirm.ack(4, false);
    check(f.handler->inflight_.size() == Handler::MaxInflight && f.handler->pending_.empty(),
          "a confirm opens one slot and drains the next pending publication");
}

void negativeConfirmRetention() {
    Fixture f; f.connect();
    int completed = 0;
    f.handler->schedulePublish("sql_queue", "stable-job", [&](bool ok) { completed += ok; });
    f.loop.flush();
    f.handler->producerChannel_->confirm.nack(1, false, false);
    check(f.handler->recovering_ && f.handler->outstanding_ == 1 && completed == 0,
          "broker NACK retains admission and withholds completion");
    f.reconnect();
    check(AMQP::TcpChannel::published.size() == 2 && AMQP::TcpChannel::published[0].body ==
          AMQP::TcpChannel::published[1].body && f.handler->inflight_.size() == 1,
          "reconnect resends the unconfirmed body with the stable job identity");
    f.handler->producerChannel_->confirm.ack(1, false);
    check(f.handler->outstanding_ == 0 && completed == 1, "resent publication completes only after a new broker confirm");
}

void generationAndOnce() {
    MQManager::AckFn oldAck, newAck;
    Fixture f([&](const std::string&, uint64_t, MQManager::AckFn ack) {
        if (!oldAck) oldAck = std::move(ack); else newAck = std::move(ack);
    });
    f.connect();
    f.handler->onConsumerMessage(AMQP::Message("old"), 17, false);
    f.handler->connectionFailed(); f.reconnect();
    oldAck(Disposition::Ack); f.loop.flush();
    check(AMQP::TcpChannel::acked == 0, "old-generation worker result cannot ACK a reused delivery tag");
    f.handler->onConsumerMessage(AMQP::Message("new"), 17, true);
    newAck(Disposition::Ack); newAck(Disposition::Retry); f.loop.flush();
    check(AMQP::TcpChannel::acked == 1 && f.loop.timers.empty(), "a worker can resolve its delivery only once");
}

void retryAndQuarantine(Disposition result, const std::string& body, const std::string& queue, int attempt) {
    Fixture f([result](const std::string&, uint64_t, MQManager::AckFn ack) { ack(result); });
    f.connect();
    f.handler->onConsumerMessage(AMQP::Message(body), 42, false);
    f.loop.flush();
    check(AMQP::TcpChannel::published.empty() && AMQP::TcpChannel::acked == 0,
          "consumer completion schedules retry/quarantine without early original ACK");
    f.loop.tick();
    const auto& published = AMQP::TcpChannel::published;
    check(published.size() == 1 && published.back().queue == queue && published.back().mode == 2,
          "replacement/quarantine is published persistently to the expected queue");
    if (attempt >= 0) check(nlohmann::json::parse(published.back().body).at("_attempt") == attempt,
                             "retry increments only the structured attempt counter");
    check(published.size() == 1 && (published.back().flags & AMQP::mandatory),
          "replacement/quarantine publish requests mandatory routing");
    check(AMQP::TcpChannel::acked == 0, "successful local publish still cannot ACK the original");
    f.handler->producerChannel_->confirm.ack(1, false);
    check(AMQP::TcpChannel::acked == 1 && AMQP::TcpChannel::acknowledgments.back().tag == 42,
          "broker-confirmed replacement/quarantine permits original delivery ACK");
}

void returnedMessage() {
    Fixture f([](const std::string&, uint64_t, MQManager::AckFn ack) { ack(Disposition::Reject); });
    f.connect();
    f.handler->onConsumerMessage(AMQP::Message("quarantine-me"), 22, false);
    f.loop.flush(); f.loop.tick();
    const auto returned = f.handler->producerChannel_->returned.received;
    check(bool(returned), "producer installs the AMQP bounced-message callback");
    if (returned) returned(AMQP::Message("quarantine-me"), 312, "NO_ROUTE");
    f.handler->producerChannel_->confirm.ack(1, false);
    check(f.handler->outstanding_ == 1 && AMQP::TcpChannel::acked == 0,
          "return followed by publisher ACK retains the item and never ACKs the original");
    if (returned) {
        f.reconnect();
        check(f.handler->inflight_.size() == 1 && AMQP::TcpChannel::published.size() == 2,
              "returned publication is retried after destination redeclaration");
    }
}

void failedPublishRetention() {
    Fixture f; f.connect();
    int completed = 0;
    AMQP::TcpChannel::allowPublish = false;
    f.handler->schedulePublish("sql_queue", "network-failed-job",
        [&](bool ok) { completed += ok; });
    f.loop.flush();
    check(f.handler->pending_.size() == 1 && f.handler->outstanding_ == 1 && completed == 0,
          "failed local publish retains its body and withholds completion");
    AMQP::TcpChannel::allowPublish = true;
    f.reconnect();
    check(f.handler->inflight_.size() == 1 &&
          AMQP::TcpChannel::published.back().body == "network-failed-job" &&
          AMQP::TcpChannel::published.back().accepted,
          "failed local publish is resent successfully after reconnect");
    f.handler->producerChannel_->confirm.ack(1, false);
    check(completed == 1 && f.handler->outstanding_ == 0,
          "network-failed publication completes only after the new broker confirm");
}

void staleRetryTimer() {
    Fixture f([](const std::string&, uint64_t, MQManager::AckFn ack) { ack(Disposition::Retry); });
    f.connect();
    f.handler->onConsumerMessage(AMQP::Message("{\"_attempt\":0}"), 88, false);
    f.loop.flush();
    f.handler->connectionFailed();
    f.reconnect();
    check(AMQP::TcpChannel::published.empty() && AMQP::TcpChannel::acked == 0,
          "old-generation retry timer cannot publish or ACK on the new connection");
}

void shutdownAndLateResult() {
    MQManager::AckFn late;
    {
        Fixture f([&](const std::string&, uint64_t, MQManager::AckFn ack) { late = std::move(ack); });
        f.connect();
        f.handler->onConsumerMessage(AMQP::Message("late-worker"), 99, false);
        f.handler->connectionFailed(); f.loop.flush();
        check(!f.loop.timers.empty(), "failure has scheduled a reconnect timer");
        f.handler->stop(); f.loop.flush(); f.loop.tick();
        check(f.loop.timers.empty() && !f.handler->connection_ && AMQP::TcpConnection::live == 0,
              "shutdown cancels reconnect and retires all AMQP resources");
        check(!f.handler->schedulePublish("sql_queue", "after-stop"),
              "shutdown rejects subsequent publication admission");
    }
    late(Disposition::Ack);
    check(AMQP::TcpChannel::acked == 0, "late worker completion is harmless after Handler and EventLoop destruction");
}

void admissionBound() {
    Fixture f; f.connect(false);
    std::atomic<size_t> accepted{0};
    std::vector<std::thread> threads;
    for (int worker = 0; worker < 4; ++worker) threads.emplace_back([&] {
        for (int i = 0; i < 3000; ++i) accepted += f.handler->schedulePublish("sql_queue", "bounded");
    });
    for (auto& thread : threads) thread.join();
    check(accepted == Handler::MaxPending && f.handler->outstanding_ == Handler::MaxPending,
          "four concurrent publishers cannot exceed process admission capacity");
    f.loop.flush();
    check(f.handler->pending_.size() == Handler::MaxPending && f.handler->inflight_.empty(),
          "accepted disconnected publications remain retained in the bounded pending queue");
    check(!f.handler->schedulePublish("sql_queue", "overflow"), "full admission reports rejection to its caller");
}

void retryAdmissionFailure() {
    Fixture f([](const std::string&, uint64_t, MQManager::AckFn ack) { ack(Disposition::Retry); });
    f.connect();
    for (size_t i = 0; i < Handler::MaxPending; ++i) f.handler->schedulePublish("sql_queue", "occupied");
    f.loop.flush();
    f.handler->onConsumerMessage(AMQP::Message("{\"_attempt\":0}"), 91, false);
    f.loop.flush(); f.loop.tick();
    check(AMQP::TcpChannel::acked == 0 && AMQP::TcpChannel::rejected == 1 &&
          AMQP::TcpChannel::rejections.back().tag == 91 &&
          (AMQP::TcpChannel::rejections.back().flags & AMQP::requeue),
          "failed replacement admission requeues the original and never ACKs it");
}
}

int main() {
    callbackLifetime(); confirmsAndWindow(); negativeConfirmRetention(); generationAndOnce();
    retryAndQuarantine(Disposition::Retry, "{\"job_id\":\"stable\",\"_attempt\":0}", "sql_queue", 1);
    retryAndQuarantine(Disposition::Retry, "{\"job_id\":\"stable\",\"_attempt\":5}", "sql_queue.dead", 5);
    retryAndQuarantine(Disposition::Reject, "invalid legacy payload", "sql_queue.dead", -1);
    returnedMessage(); failedPublishRetention(); staleRetryTimer(); shutdownAndLateResult();
    admissionBound(); retryAdmissionFailure();
    check(muduo::net::Channel::badDestruction == 0 && AMQP::TcpConnection::destroyedDuringProcess == 0,
          "all callback and registered-resource destruction invariants hold");
    std::cout << "checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
