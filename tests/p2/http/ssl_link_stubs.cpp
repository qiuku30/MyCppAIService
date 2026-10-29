// TLS is disabled in these tests. These link-only definitions fail if entered.
#include "ssl/SslConnection.h"
#include <cstdlib>

namespace ssl {
SslContext::SslContext(const SslConfig& config) : ctx_(nullptr), config_(config) { std::abort(); }
SslContext::~SslContext() = default;
bool SslContext::initialize() { std::abort(); }
SslConnection::SslConnection(const TcpConnectionPtr&, SslContext*) { std::abort(); }
SslConnection::~SslConnection() = default;
void SslConnection::startHandshake() { std::abort(); }
void SslConnection::onRead(const TcpConnectionPtr&, BufferPtr, muduo::Timestamp) { std::abort(); }
}
