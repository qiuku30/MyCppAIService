#include <memory>
#include <mutex>
#include <atomic>
#include <queue>
#include <map>
#include <deque>
#include <functional>
#include <amqpcpp.h>
#include <muduo/net/Channel.h>
#define private public
#include "AIApps/ChatServer/include/AIUtil/MQManager.h"
#undef private
#include <iostream>
int main(){int fails=0;auto check=[&](bool ok,const char* name){std::cout<<(ok?"PASS ":"FAIL ")<<name<<"\n";fails+=!ok;};
 muduo::net::EventLoop loop;
 auto h=std::make_shared<MQManager::Handler>(&loop,AMQP::Address("mock"),"sql_queue",nullptr);
 h->connect();h->monitor(h->connection_.get(),10,AMQP::readable);h->monitor(h->connection_.get(),11,AMQP::readable);
 check(muduo::net::Channel::active.size()==2,"AMQP resolver and socket fds coexist");
 h->monitor(h->connection_.get(),10,0);loop.flush();check(muduo::net::Channel::active.count(11)==1,"unmonitor old fd preserves other socket");
 h->monitor(h->connection_.get(),11,0);loop.flush();check(muduo::net::Channel::badDestruction==0,"registered channels removed before destruction");
 h->onConnected(h->connection_.get());h->schedulePublish("sql_queue","one");loop.flush();
 check(!AMQP::TcpChannel::published.empty()&&AMQP::TcpChannel::published.back().mode==2,"messages use persistent delivery mode");
 check(bool(h->producerChannel_->confirm.ack),"publisher confirms installed");
 h->disconnect();loop.flush();h.reset();loop.flush();
 std::cout<<"failures="<<fails<<"\n";return fails?1:0;
}
