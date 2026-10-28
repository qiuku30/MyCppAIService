# 项目技术栈学习清单（含详细解答）

> 目标：不是精通每个技术，是"面试能讲清楚原理 + 看代码能懂每一行"
> 方法：每学一个技术，带着你项目里已有的代码去读源码，不要空学

---

## 1. muduo 网络库（🔴 最优先，2 天）

### 1.1 Reactor 模式和 EventLoop

#### Q1: Reactor 是什么意思？和 Proactor 有什么区别？

**Reactor（反应器）**：一个线程死循环跑 `epoll_wait()`，等事件来了（可读/可写/出错），调用对应的回调函数处理。流程是：

```
epoll_wait() 阻塞等待 → 有事件到来 → epoll 返回哪些 fd 就绪 → 逐个调回调
```

**和 Proactor 的核心区别在于谁干 I/O**：

| | Reactor | Proactor |
|------|---------|---------|
| I/O 操作谁执行 | **同步 I/O**：回调里你自己调 `read()/write()` | **异步 I/O**：OS 内核帮你读完/写完，完成后通知你 |
| muduo 是哪种 | Reactor（同步非阻塞） | — |
| 典型实现 | muduo、Netty、Nginx | IOCP（Windows）、Boost.Asio |

举例：一个连接可读了，Reactor 通知你说"fd 可读了"，然后**你自己在回调里调 `read()` 把数据读出来**。Proactor 是"数据我已经帮你读好了，这是数据，你用吧"。

#### Q2: "one loop per thread" 怎么理解？

每个 EventLoop 对象绑定一个线程，一个线程只跑一个 EventLoop。这个线程的工作就是：

```
while (running_) {
    epoll_wait(...);          // 阻塞等事件
    for (每个就绪的 fd) {
        调用对应的 Channel 回调;  // 比如 onMessage()
    }
    doPendingFunctors();      // 处理其他线程投递过来的任务
}
```

**好处**：
- 单个线程内所有操作都是串行的，不需要加锁
- 每个连接的 `onMessage` 总是在同一个线程执行，连接状态不用考虑并发
- 多线程就多开几个 EventLoop，每个线程跑一个 loop

**不是**一个连接一个线程——那样 10000 个连接就 10000 个线程，内存和切换开销爆炸。muduo 是一个线程服务多个连接，所有连接的 I/O 事件由这一个线程的 `epoll_wait` 统一管理。

#### Q3: EventLoop::loop() 在干什么？

```cpp
void EventLoop::loop() {
    while (!quit_) {
        activeChannels_.clear();
        pollReturnTime_ = poller_->poll(kPollTimeMs, &activeChannels_);
        // ↑ epoll_wait 阻塞，返回就绪的 Channel 列表
        for (Channel* ch : activeChannels_) {
            currentActiveChannel_ = ch;
            ch->handleEvent(pollReturnTime_);  // 调对应回调
        }
        doPendingFunctors();  // 执行其他线程投递的任务（跨线程 send 的关键！）
    }
}
```

核心三步：
1. `poll()` → 阻塞等事件
2. 遍历就绪的 Channel → 调各自的回调
3. `doPendingFunctors()` → 处理跨线程投递的任务

#### Q4: muduo 为什么用 level-triggered 而不是 edge-triggered？

**Level-triggered（水平触发）**：只要 fd 缓冲区还有数据没读完，`epoll_wait` 就一直返回这个 fd。

**Edge-triggered（边缘触发）**：只在缓冲区从空变成有新数据的那一瞬间通知一次。

muduo 选 LT 的原因：
- LT 配合 Buffer 使用更安全——你不需要一次读干净所有数据，读到哪算哪，剩的下次再读
- ET 要求你必须一次 `read()` 到 `EAGAIN`，否则再也不会收到通知——编程更容易出错
- muduo 的 Buffer 设计就是"积攒数据 + 逐步解析"，天然适合 LT

**在你的项目里的体现**：`HttpContext::parseRequest()` 每次被调时不假设数据完整——Buffer 里可能有半行、半个 body。状态机通过 `hasMore` 控制，数据不够就返回，等下次 `onMessage` 再触发时继续解析。这就是 LT 的便利之处。

---

### 1.2 TcpServer / TcpConnection / Buffer 三件套

#### Q1: TcpServer 怎么接受新连接？

流程：

```
TcpServer::start()
  → Acceptor::listen()                     // 创建 listen fd，绑定端口
  → Acceptor 内部有一个 acceptChannel_     // 把 listen fd 注册到 epoll
  → 有新连接到来时 epoll 通知
  → Acceptor::handleRead()                 // 调 accept() 拿到 conn fd
  → new TcpConnection(loop, connFd)        // 创建 TcpConnection 对象
  → 调用户设置的 connectionCallback_       // 用户可以在里面做 SSL handshake
```

**在你的项目里**：

```cpp
// HttpServer.cpp — 连接建立时
void HttpServer::onConnection(const TcpConnectionPtr& conn) {
    if (conn->connected()) {
        // ① SSL 握手（如果启用了）
        // ② 创建 HttpContext 绑定到 conn（通过 setContext）
    }
}
```

#### Q2: TcpConnection 的生命周期？

```
[连接建立]
  → Acceptor 接受新连接
  → 创建 TcpConnection 对象
  → conn->connected() == true
  → 触发 onConnection 回调（用户在这里做初始化，如创建 HttpContext）

[数据传输]（循环）
  → epoll 通知 fd 可读
  → TcpConnection::handleRead()
    → 从 fd read() 数据到 inputBuffer_
    → 调用户的 messageCallback_（你的 onMessage）
    → 用户在回调里解析数据、生成响应
    → 用户调 conn->send(response)
      → sendInLoop() 直接 write()，或放入 outputBuffer_ 等下次可写

[连接关闭]
  → 对方关闭连接（read 返回 0）或主动调 shutdown()
  → TcpConnection::handleClose()
  → 触发 onConnection 回调（conn->connected() == false）
  → 从 EventLoop 注销 fd
  → TcpConnection 对象析构
```

#### Q3: Buffer 为什么需要？

TCP 是字节流协议，没有消息边界。一次 `read()` 可能：
- 只读到半行 HTTP 请求头
- 一次读到两个完整的 HTTP 请求
- body 分三个包到达

Buffer 的作用就是**在用户态暂存数据**，让上层代码可以按逻辑单位（行、body）来消费：

```cpp
// 你的项目里 HttpContext::parseRequest 就在做这件事：
buf->findCRLF();           // 在 Buffer 里找到第一个 \r\n
buf->peek();               // 查看数据但不消费
buf->retrieveUntil(crlf+2);// 消费已解析的数据（移动读指针）
buf->readableBytes();      // 还剩多少未读数据
```

**没有 Buffer 的话**：每次收到数据你要手动维护指针、手动拼接、处理半包——代码复杂度会翻倍。

---

### 1.3 跨线程 send() 线程安全（最关键的面试点）

#### Q1: 为什么 conn->send() 可以在任意线程调用？

muduo 的 `TcpConnection::send()` 实现里有一个关键判断：

```cpp
void TcpConnection::send(const void* data, size_t len) {
    if (loop_->isInLoopThread()) {
        // 情况 A：当前在 IO 线程 → 直接 sendInLoop()
        sendInLoop(data, len);
    } else {
        // 情况 B：当前不在 IO 线程 → 把任务投递到 IO 线程
        loop_->runInLoop([this, data, len]() {
            sendInLoop(data, len);
        });
    }
}
```

**你项目里的 SSD 流式场景就是情况 B**：

```cpp
// ChatSendHandler.cpp:76-101
std::thread([conn]() {
    // 这个线程不是 muduo IO 线程！
    conn->send("data: ...");  // ← 跨线程调用
}).detach();
```

#### Q2: eventfd 怎么工作的？

`eventfd` 是 Linux 的一个系统调用，创建一个虚拟 fd，可以跨线程做事件通知。

```
muduo IO 线程                        你的 detach 推理线程
    │                                      │
    │  epoll_wait(epfd, ...)               │
    │  ← 阻塞中...                         │
    │                                      │ conn->send(data)
    │                                      │   → runInLoop(task)
    │                                      │     → queueInLoop(task)
    │                                      │       → pendingFunctors_.push_back(task)
    │                                      │       → wakeup()
    │                                      │         → ::write(wakeupFd_, 1) ← 写 eventfd
    │                                      │
    │  epoll_wait 被 eventfd 唤醒 ◄─────────┘
    │  → 处理所有就绪的 fd（可能为空，因为只唤醒了 eventfd）
    │  → doPendingFunctors()
    │    → 从 pendingFunctors_ 取出 task
    │    → task() → sendInLoop(data) → ::write(connFd, data)
    │    → 数据真正写入 TCP socket
```

**整个流程的关键**：推理线程从不直接操作 socket fd。它只是把"要发送的数据 + 怎么发"打包成一个任务，通过 eventfd 通知 IO 线程来执行。实际的所有 I/O 操作都在 IO 线程内串行执行——没有竞态条件，不需要加锁。

#### Q3: 为什么 IO 线程不能被阻塞？

muduo 的 IO 线程服务的是**一个 EventLoop 上的所有连接**——可能几百个甚至上千个。如果 IO 线程在处理某个连接的 `onMessage` 回调时阻塞了 5 秒，那么这 5 秒内：

- 所有其他连接的可读事件得不到处理
- 所有跨线程投递的任务得不到执行（`doPendingFunctors` 被卡住了）
- 用户的响应发不出去

**所以**：IO 线程的宗旨是 **"快速处理，尽早返回"**。任何耗时操作（如数据库查询、AI 推理、文件 I/O）必须放到其他线程执行。这就是为什么你的项目里 SSE 推理要 `std::thread(...).detach()`。

---

## 2. libcurl（🟡 中等，半天）

### 2.1 同步 CURL

#### Q1: `curl_easy_perform()` 是同步还是异步？

**同步阻塞**。调用后会一直等到服务器返回完整 HTTP 响应才返回。内部流程：

```
curl_easy_perform(curl)
  ├─ ① DNS 解析
  ├─ ② TCP 连接
  ├─ ③ 发送 HTTP 请求
  └─ ④ 循环接收响应数据
       ├─ TCP 来了一段数据
       ├─ 调 CURLOPT_WRITEFUNCTION 回调 ← 数据追加到你的 buffer
       ├─ TCP 又来了一段数据
       ├─ 调 CURLOPT_WRITEFUNCTION 回调 ← 继续追加
       └─ 直到 HTTP 响应完整（Content-Length 够了 或 Connection: close）
  └─ ⑤ 返回 CURLE_OK / 错误码
```

虽然内部多次调你的回调，但 `curl_easy_perform()` 本身是**全程阻塞调用线程的**——调用返回时，完整的 HTTP 响应已经在你 buffer 里了。

#### Q2: CURLOPT_WRITEFUNCTION 回调触发时机？

每次 curl 从 TCP socket 收到一段数据，就调一次回调。不保证行边界——可能一次回调拿到 128 字节（包含 3 行），也可能拿到 16 字节（半行）。

#### Q3: CURLOPT_WRITEDATA 干什么？

传给回调函数的 `userp` 参数——就是上下文指针。你的项目里传的是 `&readBuffer`（同步模式）或 `&state`（流式模式的 StreamParseState）。

---

### 2.2 流式 SSE vs 同步

#### Q1: 同步 curl 的瓶颈在哪？

必须等完整 HTTP 响应回来才能解析。但 SSE 场景下，服务器是"边生成边发送"的——如果你等整个响应回来，在用户看来就是卡了 30 秒然后突然蹦出一大段文字。不仅体验差，而且 HTTP 连接可能因为超时断开（服务器推 token 之间没有数据传输）。

#### Q2: 流式 curl 怎么做到实时？

**同步和流式用的是同一个 WRITEFUNCTION 机制**，区别在于你在回调里做什么：

- 同步模式：回调里只是 `buffer->append(data)`，攒着。`curl_easy_perform()` 返回后一次性 `json::parse(buffer)`。
- 流式模式：回调里**立刻处理**数据——按行切、解析 JSON、提取 delta、推送给上层。

```
同步模式的 curl_easy_perform():
  [回调1] → append → [回调2] → append → ... → [回调N] → append
  → 返回
  → 你一次性解析 buffer  ← 所有数据已经在这儿了

流式模式的 curl_easy_perform():
  [回调1] → append + 找行 + 解析 + on_chunk("你")
  [回调2] → append + 找行 + 解析 + on_chunk("好")
  ...
  [回调N] → append + 找行 + 解析 + on_chunk("！")
  → 返回
```

#### Q3: 为什么要维护跨回调的 buffer？

因为 curl 给的数据可能不包含完整的 `\n`，比如：

```
回调1 给你:  "data: {\"choices\":[{\"de"    ← 没有 \n，JSON 也不完整
回调2 给你:  "lta\":{\"content\":\"你\"}}]}\n"
```

如果你在每个回调里直接解析：

```cpp
// ❌ 错误做法：每个回调独立解析
std::string chunk = static_cast<char*>(contents);  // 只拿到 "data: {\"choices\":[{\"de"
// 找不到完整的 JSON → 解析失败 → 这行数据丢了！
```

`StreamParseState::buffer` 就是解决这个问题的——把两个回调的数据拼起来再找 `\n`：

```cpp
// ✅ 正确做法
st->buffer.append(chunk);   // buffer = "data: {\"choices\":[{\"delta\":{\"content\":\"你\"}}]}\n"
// 现在找 \n → 拿到完整的一行 → 解析 JSON → 提取 "content" → on_chunk("你")
```

---

## 3. RabbitMQ / AMQP（🟡 中等，1 天）

### 3.1 AMQP 核心概念详解

#### AMQP 模型全景

```
Producer                        Broker                        Consumer
   │                              │                              │
   ├─ Connection ─┐               │               ┌─ Connection ─┤
   │  ├─ Channel1 ─┤── publish ──►│── Exchange ──►│── Queue ──►├─ Channel1 │
   │  ├─ Channel2 ─┤              │    (路由)     │   (缓冲区)   ├─ Channel2 │
   │  └─ Channel3 ─┘              │               │             └─ Channel3 │
   └──────────────────────────────┴───────────────┴──────────────────────────┘
```

#### 核心概念详解

**Connection（TCP 连接）**：物理 TCP 连接，比较重（每次建立要握手）。你的项目和 Broker 之间通常只建立 1-2 条 Connection。

**Channel（信道）**：在一条 TCP 连接上开的虚拟连接。创建/销毁很轻量，协议层面用 channel_id 区分。**因为共享同一条 TCP 连接，所以多个 Channel 发消息时必须在应用层加锁**——否则两个线程同时在同一个 Channel 上调用 `BasicPublish`，AMQP 帧会在 TCP 层交错，Broker 解析出乱码。

**Exchange（交换机）**：消息路由规则。类型：
- `direct`：按 routing key 精确匹配
- `fanout`：广播到所有绑定的队列
- `topic`：按模式匹配（`stock.nyse.*`）

你的项目用的应该是 `direct` 或默认 exchange（空名字的 direct exchange）。

**Queue（队列）**：消息缓冲区。生产者发到 Exchange，Exchange 路由到 Queue，消费者从 Queue 取。

**Binding**：Exchange 和 Queue 之间的绑定关系。

#### 为什么 Channel 不是线程安全的？

AMQP 协议规定消息帧结构为：`[type, channel_id, payload_length, payload, end_marker]`。如果两个线程同时在同一个 channel_id 上发消息，帧会在 TCP 流中交错：

```
线程A: [type=1, ch=1, len=100, payload_A..., end]
线程B: [type=1, ch=1, len=50,  payload_B..., end]
交错了: [type=1, ch=1, len=100, pa][type=1, ch=1, len=50, yload_A...]
         ↑ Broker 解析失败：帧格式错误
```

**所以 SimpleAmqpClient 不是线程安全的，你的项目给每个 Channel 配了一个 `std::timed_mutex`**——拿到锁的线程才能在这个 Channel 上操作。

---

### 3.2 ACK / NACK / QoS 详解

#### Q1: 自动 ACK vs 手动 ACK

**自动 ACK（no_ack=true）**：Broker 把消息发给消费者后**立刻**标记"已消费"，不管消费者的实际处理结果。如果消费者处理到一半 crash 了，消息就丢了。

**手动 ACK（no_ack=false）**：消费者处理完消息后**自己调 `BasicAck()`** 告诉 Broker。如果消费者 crash 了没有 ACK，Broker 会把消息重新投递（requeue）给其他消费者。

你的项目用手动 ACK，因为消息是 SQL 语句——**必须确认插入数据库成功才能 ACK**。

#### Q2: NACK 什么时候用？

处理失败 + 错误是**暂时的**（如网络超时、数据库临时不可用）→ NACK + requeue，让消息重新入队，下次可能就成功了。

处理失败 + 错误是**永久的**（如 SQL 语法错误）→ **也 ACK**（NACK 不 requeue），因为重试 100 次也是同样的错误。

#### Q3: basic.qos(prefetch_count) 干什么？

限制消费者一次能持有多少条**未 ACK 的消息**。`prefetch_count=1` 的意思是：Broker 给消费者发一条消息后，必须等它 ACK，才会发下一条。

**为什么你的项目设 prefetch=1？**

```
prefetch=100 时（坏）：
  消费者A: [消息1] [消息2] [消息3] ... [消息100]  ← 全堆在A这，忙不过来
  消费者B:  闲着  ← 没消息可消费！

prefetch=1 时（好）：
  消费者A: [消息1] → 处理 → ACK → 拿到消息3
  消费者B: [消息2] → 处理 → ACK → 拿到消息4
  → 自动负载均衡
```

prefetch=1 配合手动 ACK 实现了"能者多劳"的负载均衡：处理快的消费者 ACK 快，就能拿到更多消息。

---

### 3.3 生产者池和消费者池详解

#### 生产者池

**为什么多个 Channel**：提高并发——如果只有一个 Channel，所有生产者线程排队等一把锁。多个 Channel 让生产者分散到不同的锁上。

**轮询选 Channel**：

```cpp
// 原子变量实现 round-robin
static std::atomic<int> channel_index_{0};
int idx = channel_index_.fetch_add(1) % channels_.size();
// fetch_add 是原子操作，返回旧值，然后自增
// 线程1: fetch_add → 返回0, atom 变成1 → 选 Channel[0]
// 线程2: fetch_add → 返回1, atom 变成2 → 选 Channel[1]
// 线程3: fetch_add → 返回2, atom 变成3 → 选 Channel[0]（轮了一圈）
```

**超时降级**：选好 Channel 后尝试加锁——`timed_mutex::try_lock_for(100ms)`：
- 拿到锁 → publish → 解锁 → 返回成功
- 没拿到锁（超时了）→ **不在这棵树上吊死** → 遍历其他 Channel
- 所有 Channel 都忙 → 返回失败

**为什么不用普通 mutex**：`std::mutex::lock()` 是死等——如果另一个线程正在同一个 Channel 上执行大 SQL（耗时几秒），调用者就被卡几秒。`try_lock_for` 让你能设一个容忍上限，超了就换路。

#### 消费者池

**一个消费者独占一个 Channel**：消费者线程启动时创建自己的 Connection + Channel，长期持有。不共享，所以消费者不需要给 Channel 加锁。

**断线自动重连**：

```cpp
void MQManager::consumeLoop(int worker_id) {
    while (running_) {
        try {
            // ① 创建连接
            auto conn = AmqpClient::Channel::Create(host, port, user, pass);
            // ② 声明队列
            conn->DeclareQueue(queue_name, false, true, false, false);
            // ③ 设置 prefetch
            conn->BasicQos(0, 1, false);  // prefetch=1
            // ④ 开始消费（阻塞在这，直到连接断开或异常）
            conn->BasicConsume(queue_name, consumer_tag);
            // ⑤ 收到消息 → 回调 → 执行 SQL → 成功 ACK / 失败 ACK（不可恢复）
        } catch (const std::exception& e) {
            LOG_ERROR << "Consumer " << worker_id << " disconnected: " << e.what();
            std::this_thread::sleep_for(std::chrono::seconds(5));  // 等 5 秒重连
        }
    }
}
```

**错误分类**：

```cpp
// 回调里
try {
    db->executeUpdate(sql);  // 执行 SQL
    channel->BasicAck(envelope);  // 成功 → ACK
} catch (const sql::SQLException& e) {
    // SQL 语法错误 → 不可恢复 → ACK（丢弃，记录日志等人工排查）
    LOG_ERROR << "SQL Error: " << e.what() << " | SQL: " << sql;
    channel->BasicAck(envelope);  // ACK 不 requeue
} catch (const std::exception& e) {
    // 网络/连接异常 → 可恢复 → NACK + requeue
    channel->BasicNack(envelope, false, true);
    throw;  // 抛出到外层 catch，触发重连
}
```

---

## 4. MySQL / mysqlcppconn（🟢 较轻，半天）

### 4.1 PreparedStatement 和 ResultSet 的生命周期

#### Q1: PreparedStatement 为什么比拼接字符串安全？

**拼接字符串（SQL 注入风险）**：

```cpp
// ❌ 恶意输入
std::string userName = "'; DROP TABLE users; --";
std::string sql = "SELECT * FROM users WHERE name = '" + userName + "'";
// 结果: SELECT * FROM users WHERE name = ''; DROP TABLE users; --'
```

**PreparedStatement（参数分离）**：

```cpp
// ✅ 参数和 SQL 结构分离
PreparedStatement* stmt = conn->prepareStatement("SELECT * FROM users WHERE name = ?");
stmt->setString(1, userName);  // 参数值被安全转义，不会被当成 SQL 代码执行
ResultSet* rs = stmt->executeQuery();
```

**底层原理**：PreparedStatement 先把 SQL 模板发给 MySQL 服务器预编译，参数值在第二步单独发送。MySQL 知道哪个位置是参数，不会把参数内容当成 SQL 语法解析。

#### Q2: ResultSet 依赖谁？

**ResultSet 依赖 PreparedStatement 活着！**

这是 mysqlcppconn 的一个接口设计陷阱：

```cpp
class PreparedStatement {
public:
    virtual ResultSet* executeQuery() = 0;
    // 注意：返回的是裸指针 ResultSet*
    // ResultSet 内部的元数据和缓冲由 PreparedStatement 持有
    // PreparedStatement 析构 → ResultSet 指向的内存变成野指针
};
```

**你踩坑的代码（修复前）**：

```cpp
// ❌ Bug: stmt 析构后 ResultSet 变野指针
sql::ResultSet* executeQuery(const std::string& sql, Args... args) {
    auto stmt = conn->prepareStatement(sql);  // unique_ptr
    bindParams(stmt, args...);
    sql::ResultSet* res = stmt->executeQuery();
    return res;  // stmt 在这行之后析构！res 变成野指针！
}
```

**修复后**：

```cpp
// ✅ 用 lastStmt_ 成员持有 stmt，不让其析构
sql::ResultSet* res = stmt->executeQuery();
lastStmt_ = std::move(stmt);  // 所有权转移到成员变量
return res;  // 安全：lastStmt_ 还活着，ResultSet 可用
// 下一次 executeQuery 时 lastStmt_ 被覆盖，旧的 stmt 才释放
```

**为什么 PreparedStatement 是抽象类**：mysqlcppconn 编译时选择不同的驱动（MySQL 原生 / JDBC）。接口是虚的，具体实现在编译链接时由驱动库提供。所以你不能"直接看源码知道 PreparedStatement 和 ResultSet 的关系"——这是运行时的事，得靠经验或踩坑才能发现。

---

### 4.2 连接池的设计点详解

#### 完整的 getConnection 流程

```
用户调 getConnection()
  │
  ├─ ① 加锁，检查连接队列
  │    ├─ 队列有连接 → 取出队首的 shared_ptr<DbConnection>
  │    └─ 队列空 → cv_.wait_for(30s)
  │         ├─ 30s 内被唤醒 → 重新检查队列
  │         └─ 30s 超时 → 抛异常 "Connection pool exhausted"
  │
  ├─ ② 健康检查
  │    ├─ conn->ping() → true → 连接活着，继续
  │    ├─ conn->ping() → false → conn->reconnect() 尝试恢复
  │    │    ├─ reconnect 成功 → 继续
  │    │    └─ reconnect 失败 → createConnection() 创建新连接替换
  │    └─ 连 createConnection 都失败 → 抛异常
  │
  └─ ③ 返回 shared_ptr(conn.get(), deleter)
       └─ 引用计数归零时执行 deleter → 连接 push 回队列
```

#### 后台心跳检测

```cpp
void checkConnections() {
    while (true) {
        // ① 加锁，把队列拷贝到局部变量（复制 shared_ptr，引用计数+1）
        std::vector<std::shared_ptr<DbConnection>> connsToCheck;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            auto temp = connections_;
            while (!temp.empty()) {
                connsToCheck.push_back(temp.front());
                temp.pop();
            }
        }  // 解锁！后面 ping 不持有锁

        // ② 在锁外 ping（ping 是网络操作，可能很慢，不能占着锁）
        for (auto& conn : connsToCheck) {
            if (!conn->ping()) {
                try {
                    conn->reconnect();
                } catch (...) {
                    LOG_ERROR << "Reconnect failed";
                }
            }
        }

        // ③ 休眠 60 秒，避免频繁检查消耗 CPU
        sleep(60);
    }
}
```

**为什么不检查正在使用的连接？** 因为 `connections_` 队列只存放空闲连接。正在使用的连接（被 shared_ptr 持有）不在队列里。如果使用中的连接断开了，会在用户还回时被 `getConnection()` 中的健康检查发现。

#### 竞态条件分析

**场景：检查线程和 getConnection 同时操作同一个连接**

```
检查线程: connsToCheck 拷贝了队列（shared_ptr 引用计数 +1）
用户线程: getConnection() 从队列 pop 同一个 shared_ptr → 队列里没了，但 shared_ptr 引用计数还没归零（检查线程还持有）
检查线程: conn->ping() → 连接活着（或 reconnect 成功）
用户线程: 拿到 shared_ptr，正常使用
→ 没冲突！因为 shared_ptr 保证了连接对象不会被提前析构
```

**如果在 getConnection 持有连接期间，检查线程退出并释放了自己的 shared_ptr 副本**——引用计数减 1 但不归零（用户线程还持有），连接对象不受影响。

---

### 4.3 shared_ptr 自动归还机制详解

#### 普通 shared_ptr vs aliasing constructor

**普通 shared_ptr**：引用计数和指向的对象是同一个。

```cpp
shared_ptr<DbConnection> p(conn);  // p 管理的是 conn 指向的对象
// p 析构 → delete conn → 这不行！连接不能 delete，要放回池里
```

**Aliasing constructor**：指向 A，但引用计数管的是 B。

```cpp
shared_ptr<DbConnection> p(conn.get(),  deleter);
//                          ↑ 指向的  ↑ 引用计数管的是什么
// p.get() 返回 conn.get()（裸指针）
// 但 p 的引用计数和 deleter 由 lambda 里的 conn（shared_ptr 副本）管理
```

在你的项目里：

```cpp
auto conn = connections_.front();  // shared_ptr<DbConnection>，引用计数=1
connections_.pop();

return std::shared_ptr<DbConnection>(
    conn.get(),            // ① get() 返回的裸指针 —— 用户通过这个访问连接
    [this, conn](DbConnection*) {  // ② conn 被 lambda 捕获！引用计数+1=2
        connections_.push(conn);   // 最终引用计数=0走回调
        cv_.notify_one();
    });
// 返回的 shared_ptr 引用计数=2（conn + lambda 捕获的 conn 副本）
// 但用户只需要 1 个引用计数（lambda 那个）来维持生命周期
```

**实际发生在内存中的结构**：

```
control_block (引用计数=2: conn + lambda捕获的副本)
    │
    ├── owned_ptr: DbConnection 对象
    │              ↑
    │              │ conn.get() 指向这里
    │              │
    │              │ 返回的 shared_ptr ── 用户拿到的，get()返回这个地址
    │              │ 用户调用 conn->executeQuery() 通过这个地址
    │
    └── deleter: lambda [this, conn](DbConnection*){ connections_.push(conn); }
                   ↑
                   当引用计数归零时调用，不 delete 而是 push 回池
```

**生命周期**：
1. 返回后，`conn` 局部变量离开作用域，引用计数从 2 降到 1（只剩 lambda 捕获的副本）
2. 用户持有的 shared_ptr 也持有一个引用计数（因为它是用 aliasing 构造的，共享同一个 control block）
3. 用户释放 shared_ptr → 引用计数归零 → 不调 `delete`，调 lambda → 连接 push 回队列

#### 异常安全

```cpp
// 假设这个函数抛异常
void riskyOperation(shared_ptr<DbConnection> conn) {
    conn->executeUpdate(sql);  // 可能抛异常
    // 如果这里 return 抛异常，conn 自动析构，连接回池
}

// 调用方
{
    auto conn = pool.getConnection();  // 拿到连接，引用计数=1
    riskyOperation(conn);              // 不管是不是抛异常
    // conn 离开作用域 → 引用计数归零 → lambda 执行 → 连接自动回池
}
```

**如果不用 shared_ptr，用手动归还**：

```cpp
{
    auto* conn = pool.getConnection();  // 裸指针
    riskyOperation(conn);               // 抛异常 → 直接跳到 catch
    pool.returnConnection(conn);        // ← 这行被跳过了！连接泄漏！
}
```

---

## 5. nlohmann/json（🟢 最快，1 小时）

### 5.1 必须掌握的 6 个操作（带项目实例）

#### ① 解析

```cpp
// 从字符串解析
json j = json::parse(R"({"name":"张三","age":25})");

// 你的项目里：解析 curl 返回的 HTTP body
json response = executeCurl(payload);
// 或解析请求体
auto j = json::parse(req.getBody());
```

#### ② 访问

```cpp
// operator[] 直接访问（key 不存在会抛异常）
std::string name = j["name"];                    

// value() 安全访问（key 不存在返回默认值）
int age = j.value("age", 0);                     

// contains() 检查 key 是否存在
bool hasCity = j.contains("city");               

// 嵌套访问 — 你的项目里提取 AI 回复内容
std::string content = j["choices"][0]["message"]["content"];
std::string delta   = j["choices"][0]["delta"]["content"];  // 流式用 delta
```

#### ③ 构建

```cpp
// 构建响应 JSON
json resp;
resp["status"] = "ok";
resp["data"] = json::array();
resp["data"].push_back({{"id", 1}, {"text", "你好"}});

// 你的项目里：构建发往 AI API 的请求体
json payload;
payload["model"] = getModel();
payload["messages"] = msgArray;
payload["stream"] = true;  // 流式
```

#### ④ 序列化

```cpp
std::string s = resp.dump();      // 无缩进（网络传输用）
std::string s = resp.dump(4);     // 4 空格缩进（调试/日志用）

// 你的项目里：
std::string payloadStr = payload.dump();  // 作为 curl POST body
```

#### ⑤ 异常处理

```cpp
try {
    json j = json::parse(aiResponse);
} catch (const json::parse_error& e) {
    // LLM 可能返回了非 JSON 格式的文本，做回退处理
    result.isToolCall = false;  
}
```

#### ⑥ 遍历

```cpp
for (auto& [key, val] : j.items()) {
    // key 是 string，val 是 json
}

// 你的项目里 AIConfig 解析工具配置
for (auto& tool : j["tools"]) {
    AITool t;
    t.name = tool.value("name", "");
    for (auto& [key, val] : tool["params"].items()) {
        t.params[key] = val.get<std::string>();
    }
}
```

---

## 6. 学习进度建议（总计 ~4.5 天）

```
Day 1 上午: muduo ~ EventLoop + Reactor 概念（1.1）
Day 1 下午: muduo ~ TcpConnection + send() 线程安全（1.2 + 1.3）
Day 2 上午: libcurl ~ 同步 + WRITEFUNCTION 回调机制（2.1 + 2.2）
Day 2 下午: RabbitMQ ~ AMQP 核心概念 + 你的 MQManager 代码（3.1 + 3.2）
Day 3 上午: RabbitMQ ~ 生产者/消费者池 + 你自己的代码（3.3）
Day 3 下午: MySQL ~ PreparedStatement + 连接池 + shared_ptr（4.1 + 4.2 + 4.3）
Day 4 上午: nlohmann/json + 串一遍简历所有点（5 + 复习）
```

---

## 7. 学完后自测题（含答案）

1. **muduo IO 线程为什么不能被阻塞？**
   
   IO 线程通过 EventLoop 的 `epoll_wait` 服务该线程上的所有连接（可能几百个）。如果 IO 线程在一个 `onMessage` 回调里阻塞了 10 秒，所有其他连接的可读事件都得不到处理，所有跨线程投递的任务也得不到执行——整个服务器卡死。

2. **conn->send() 在非 IO 线程调用时，数据怎么到达 TCP socket？**

   `send()` 内部判断当前是否在 IO 线程——如果不是，调 `runInLoop()` 把 `sendInLoop` 封装成任务 push 到 `pendingFunctors_` 队列，然后往 `eventfd` 写 1 字节唤醒 IO 线程。IO 线程从 `epoll_wait` 醒来，在 `doPendingFunctors()` 里执行真正的 `::write(fd, data)`。

3. **libcurl 同步模式下的 WRITEFUNCTION 回调，和流式模式下的回调，触发时机有什么不同？**

   触发时机完全一样——都是 curl 从 TCP socket 收到一段数据就触发。区别在于回调里做什么：同步模式攒数据，`curl_easy_perform()` 返回后一次性解析；流式模式在回调里立刻按行切、解析 JSON、提取内容、向上层推送 token。

4. **RabbitMQ 的 Channel 为什么不是线程安全的？**

   多个 Channel 共享同一条 TCP 连接。如果两个线程同时在同一个 channel_id 上发 AMQP 帧，帧会在 TCP 流中交错——Broker 解析出乱码。AMQP 协议本身不保证 Channel 级别的并发安全，所以应用层必须加锁（你的项目用 `std::timed_mutex`）。

5. **手动 ACK 和自动 ACK 的区别？什么时候该用 NACK？**
   
   - 自动 ACK：Broker 发消息后立刻标记已消费，不管消费者是否处理成功
   - 手动 ACK：消费者处理完后自己调 `BasicAck()`，Broker 才标记已消费
   - NACK：处理失败 + 错误可恢复（网络超时）→ NACK + requeue = 消息重新入队
   - 不可恢复（SQL 语法错误）：也 ACK（不 requeue），因为重试毫无意义，记录日志等人工处理

6. **PreparedStatement 析构后，它的 ResultSet 还能用吗？为什么？**

   **不能。** mysqlcppconn 中 ResultSet 的元数据和缓冲区由 PreparedStatement 持有。PreparedStatement 析构后 ResultSet 内部指针变成野指针，访问即崩溃。修复方式：用一个 `lastStmt_`（`unique_ptr<PreparedStatement>`）在 `executeQuery` 返回 ResultSet 后接管 stmt 的所有权，等业务代码用完 ResultSet 再释放。

7. **shared_ptr 的 aliasing constructor 是什么？你的连接池里怎么用它实现自动归还？**

   Aliasing constructor 创建的 shared_ptr：`get()` 返回 A 的地址，但引用计数管理的是 B 对象。在你的连接池里：返回 `shared_ptr<DbConnection>(conn.get(), [this, conn](DbConnection*){ connections_.push(conn); })`——用户拿到的是 `conn.get()` 的裸指针，但析构时执行的 deleter 是 lambda（把连接推回池），不调用 `delete`。异常安全：不管什么路径离开作用域，shared_ptr 析构一定触发 lambda，连接一定回池。

8. **你的 SSE 流式链路中，从 curl 收到数据到浏览器显示 token，经过了几层回调？**

   完整链路：
   ```
   curl WRITEFUNCTION 回调（底层，TCP 数据到达）
     → StreamParseState buffer 攒数据 + 按 \n 切行
     → AIStrategy::parseStreamChunk()（解析 JSON → 提取 delta.content）
     → executeCurlStream 的 on_chunk 回调
     → AIHelper::chatStream 的 on_token 回调（拼 full_text + 调外层的 on_token）
     → ChatSendHandler 的 on_token 回调（包装成 SSE 格式 JSON）
     → conn->send("data: ...\n\n")（muduo 跨线程 send → eventfd → IO 线程 → ::write）
     → TCP → 浏览器
   ```
   一共经过了 **5 层回调**：curl 写回调 → parseStreamChunk → on_chunk → on_token → conn->send。
