#include "AIUtil/ChatMessage.h"
#include "AIUtil/MQManager.h"
#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
int unavailable(const char* reason) {
    std::cout << json{{"driver", "real_MQManager"}, {"status", "unavailable"},
                      {"reason", reason}}.dump() << '\n';
    return 2;
}

double number(const std::string& input) {
    size_t consumed = 0;
    const double value = std::stod(input, &consumed);
    if (consumed != input.size() || !std::isfinite(value))
        throw std::invalid_argument("invalid driver argument");
    return value;
}

std::string readInput() {
    std::string body;
    std::array<char, 4096> buffer{};
    while (std::cin) {
        std::cin.read(buffer.data(), buffer.size());
        body.append(buffer.data(), static_cast<size_t>(std::cin.gcount()));
        if (body.size() > 256 * 1024)
            throw std::invalid_argument("oversized driver input");
    }
    if (!std::cin.eof()) throw std::runtime_error("driver input read failed");
    return body;
}
} // namespace

int main(int argc, char** argv) {
    try {
        int copies = 3;
        double seconds = 12.0;
        for (int index = 1; index < argc; ++index) {
            const std::string option = argv[index];
            if (++index >= argc) return unavailable("invalid_driver_arguments");
            const double value = number(argv[index]);
            if (option == "--copies" && value >= 1 && value <= 10 && std::floor(value) == value)
                copies = static_cast<int>(value);
            else if (option == "--seconds" && value >= 1 && value <= 60)
                seconds = value;
            else
                return unavailable("invalid_driver_arguments");
        }
        for (const char* key : {"RABBITMQ_HOST", "RABBITMQ_USER", "RABBITMQ_PASS"}) {
            const char* value = std::getenv(key);
            if (!value || !*value) return unavailable("missing_container_mq_configuration");
        }

        // Use the current production protocol; never log stdin/body or credentials.
        const auto exchange = ChatExchange::parse(readInput());
        const auto body = exchange.serialize();
        const std::string probeQueue = "mycpp_it_producer_probe_" + exchange.message_id;
        muduo::Logger::setLogLevel(muduo::Logger::WARN);
        muduo::net::EventLoop loop;
        auto& manager = MQManager::instance();
        struct StopBeforeLoopDestruction {
            MQManager& manager;
            ~StopBeforeLoopDestruction() { manager.shutdown(); }
        } stopBeforeLoopDestruction{manager};
        manager.start(&loop, probeQueue,
            [](const std::string&, uint64_t, MQManager::AckFn done) {
                done(MQManager::Disposition::Ack);
            });
        int admitted = 0;
        for (int index = 0; index < copies; ++index)
            if (manager.publish("sql_queue", body)) ++admitted;

        const auto began = std::chrono::steady_clock::now();
        bool stopped = false;
        loop.runAfter(seconds, [&]() {
            // Shutdown is required on the owning loop. Give deferred channel /
            // connection retirement a further iteration before loop destruction.
            manager.shutdown();
            stopped = true;
            loop.runAfter(0.05, [&]() { loop.quit(); });
        });
        loop.loop();
        if (!stopped) return unavailable("driver_event_loop_ended_before_shutdown");
        if (admitted != copies) return unavailable("driver_memory_admission_rejected");

        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - began).count();
        std::cout << json{{"driver", "real_MQManager"}, {"status", "admitted"},
                          {"requested", copies}, {"admitted", admitted},
                          {"message_id", exchange.message_id}, {"destination", "sql_queue"},
                          {"probe_queue", probeQueue}, {"loop_seconds", elapsed},
                          {"memory_admission_only", true},
                          {"publisher_confirms_observed", false}}.dump() << '\n';
        return 0;
    } catch (const std::exception&) {
        // Driver/setup/input errors are unavailable, never a product RED claim.
        return unavailable("driver_setup_input_or_runtime_failure");
    }
}
