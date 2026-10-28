#include <muduo/net/TcpServer.h>
#include <muduo/net/EventLoop.h>
#include <set>
#include <string>
#include <muduo/net/TcpClient.h> 

using namespace muduo;
using namespace muduo::net;

class chatserver1{
    public:
    chatserver1(EventLoop* loop, const InetAddress& listenAddr)
        : server_(loop, listenAddr, "chatserver1")
    {
        server_.setConnectionCallback(
            std::bind(&chatserver1::onConnection, this, std::placeholders::_1));
        server_.setMessageCallback(
            std::bind(&chatserver1::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    }
    server_.setThreadNum(4);
    void start()
    {
        server_.start();
    }

    private:
    TcpServer server_;
    EventLoop* loop_;
    std::set<TcpConnectionPtr> clients_;
};

class chatclient1{
    public:
    chatclient1(EventLoop* loop, const InetAddress& serverAddr)
        : client_(loop, serverAddr, "chatclient1")
    {
        client_.setConnectionCallback(
            std::bind(&chatclient1::onConnection, this, std::placeholders::_1));
        client_.setMessageCallback(
            std::bind(&chatclient1::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    }
    void connect()
    {
        client_.connect();
    }

    private:
    TcpClient client_;
    EventLoop* loop_;
    TcpConnectionPtr conn_;
};

int main()
{
    EventLoop loop;
    InetAddress listenAddr(8888);
    chatserver1 server(&loop, listenAddr);
    server.start();
    loop.loop();
}