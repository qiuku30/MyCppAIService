#pragma once
#include <memory>
namespace muduo { namespace net {
// Only the owning context relationship is modeled; production HTTP/session code is linked below.
class TcpConnection { public: std::shared_ptr<void> context; };
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
}}
