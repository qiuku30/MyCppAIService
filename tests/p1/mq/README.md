# MQ source regression harness

```sh
python3 tests/p1/mq/run.py normal
python3 tests/p1/mq/run.py asan
python3 tests/p1/mq/run.py tsan
```

The runner builds both the original five checks and the extended regression suite against the current `MQManager.cpp`. Each run prints its temporary evidence directory containing binaries and separate test logs. No build output is written into the repository. The compiler defaults to `clang++` (`CXX` overrides it). The harness uses the JSON header in `/private/tmp/mycpp-p1-deps`; set `MYCPP_TEST_DEPS` to the directory containing `nlohmann/json.hpp` when needed. Each binary has a 60-second timeout. The sanitizer modes use ASan/UBSan and TSan respectively.

The fake interfaces model the library contracts relevant to the tests, including thread-safe queued EventLoop callbacks, later-turn Channel destruction, constructor-time fd monitoring, three-argument publisher NACK, and the three-argument bounced-message callback. The tests invoke real production state transitions and cover:

- Independent resolver/socket descriptors, unmonitor inside a Channel callback, and process-to-error reentry.
- Single, multiple, out-of-order and duplicate confirms; retained unconfirmed data; the in-flight window; reconnect resend and local send failure.
- Old-generation consumer results and retry timers, once-only completion, shutdown cancellation, and callbacks arriving after Handler lifetime.
- Retry attempt limits, persistent quarantine, mandatory routing, return-before-ACK handling, and withholding the original ACK until a replacement is confirmed.
- Four concurrent publishers racing the 10,000-item admission limit, and requeueing the original when replacement admission fails.

The API signatures and lifecycle assumptions were checked against primary sources:

- [AMQP-CPP callback types](https://github.com/CopernicaMarketingSoftware/AMQP-CPP/blob/master/include/amqpcpp/callbacks.h), [DeferredConfirm](https://github.com/CopernicaMarketingSoftware/AMQP-CPP/blob/master/include/amqpcpp/deferredconfirm.h), and [DeferredRecall](https://github.com/CopernicaMarketingSoftware/AMQP-CPP/blob/master/include/amqpcpp/deferredrecall.h).
- [AMQP-CPP TcpConnection](https://github.com/CopernicaMarketingSoftware/AMQP-CPP/blob/master/src/linux_tcp/tcpconnection.cpp) and [resolver](https://github.com/CopernicaMarketingSoftware/AMQP-CPP/blob/master/src/linux_tcp/tcpresolver.h): monitor may be called during construction, process, or connection destruction.
- [Muduo Channel](https://github.com/chenshuo/muduo/blob/master/muduo/net/Channel.cc) and [EventLoop](https://github.com/chenshuo/muduo/blob/master/muduo/net/EventLoop.cc): Channel removal/destruction and queued-callback ordering.
- [RabbitMQ confirms](https://www.rabbitmq.com/docs/confirms#when-will-published-messages-be-confirmed-by-the-broker): an unroutable message can receive publisher ACK; mandatory returns precede that ACK.

The repository does not pin its AMQP-CPP installation version. These are current upstream contracts, not proof that `/home/qiuku/.local` has the same headers. The fake EventLoop does not perform socket polling and its timers advance only when the test asks them to. Passing this harness does not establish real broker delivery, restart durability, queue topology, Linux linking, or database behavior. The synchronous application `MessageHandler` may still block the EventLoop during database work.
