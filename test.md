# muduo 源码重学笔记

> 本文只描述当前仓库 `/home/nilv/A/muduo/-muduo` 中实际存在的实现。
> 这是一个基于 C++11、epoll 的精简版 muduo；当前没有原版 muduo 的
> `Timer`、`TimerQueue`、`Connector`、`TcpClient` 等模块。

## 0. 先建立全局模型

这个库解决的核心问题是：**少量 I/O 线程如何管理大量非阻塞 socket，并把网络事件安全地交给业务回调。**

当前 echo 服务使用一个 main loop 和三个 sub loop：

~~~text
main 线程
  EventLoop(main loop)
    ├── Poller(epoll)
    ├── wakeupChannel(eventfd)
    └── Acceptor
          └── acceptChannel(listenfd)

                  新连接按 round-robin 分配
                              │
       ┌──────────────────────┼──────────────────────┐
       ▼                      ▼                      ▼
I/O 线程 0              I/O 线程 1              I/O 线程 2
EventLoop                EventLoop                EventLoop
  └── TcpConnection        └── TcpConnection        └── TcpConnection
        ├── Socket               ...                      ...
        ├── Channel
        ├── inputBuffer
        └── outputBuffer
~~~

先记住五个所有权规则：

1. 一个 `EventLoop` 固定属于创建它的线程，一个线程最多创建一个 `EventLoop`。
2. 一个 `EventLoop` 独占一个 `Poller` 和一个用于唤醒自己的 `eventfd/Channel`。
3. 一个 `Channel` 固定属于一个 `EventLoop`，但它不拥有也不关闭 fd。
4. `Socket` 才是 fd 的 RAII 所有者；`Socket` 析构时调用 `close(fd)`。
5. `TcpConnection` 拥有连接对应的 `Socket`、`Channel` 和输入/输出缓冲区。

## 1. Reactor 主干：EventLoop、Poller、Channel

### 1.1 三个类各自只做一件事

| 类 | 职责 | 不负责什么 |
|---|---|---|
| `EventLoop` | 不断等待事件、分发活跃 Channel、执行跨线程任务 | 不解释某个 fd 的业务含义 |
| `Poller/EPollPoller` | 调用 `epoll_wait/epoll_ctl`，发现哪些 Channel 就绪 | 不执行连接或消息回调 |
| `Channel` | 保存 fd 的关注事件、实际事件及四类回调，并按事件位调用回调 | 不拥有 fd，不主动等待事件 |

因此主循环可以压缩成下面三步（`src/EventLoop.cc:77`）：

~~~cpp
while (!quit_)
{
    activeChannels_.clear();
    pollRetureTime_ = poller_->poll(kPollTimeMs, &activeChannels_);
    for (Channel *channel : activeChannels_)
        channel->handleEvent(pollRetureTime_);
    doPendingFunctors();
}
~~~

这里存在两条不同的任务来源：

- `activeChannels_`：内核发现的 I/O 事件，例如 listenfd 可读、connfd 可读/可写。
- `pendingFunctors_`：其他线程或当前回调排队提交给本 loop 的普通 C++ 函数。

这两条来源最终都只能在 `EventLoop` 所属线程里执行，从而让连接状态和缓冲区操作尽量避免加锁。

### 1.2 events_ 与 revents_ 不能混淆

`Channel` 中名字相近的两个字段方向相反：

| 字段 | 谁写入 | 含义 | 例子 |
|---|---|---|---|
| `events_` | 网络库 | 我希望 epoll 关注什么 | `EPOLLIN | EPOLLPRI` |
| `revents_` | `EPollPoller` | epoll 告诉我实际发生了什么 | `EPOLLIN` |

调用 `enableReading()` 的完整方向是：

~~~text
Channel::enableReading
  → 修改 events_
  → Channel::update
  → EventLoop::updateChannel
  → EPollPoller::updateChannel
  → epoll_ctl(ADD 或 MOD)
~~~

事件发生时则反向流动：

~~~text
epoll_wait
  → EPollPoller::fillActiveChannels
  → 写入 Channel::revents_
  → EventLoop 遍历 activeChannels_
  → Channel::handleEvent
  → read/write/close/error callback
~~~

`Channel::handleEventWithGuard()` 会依次检查 HUP、ERR、读、写事件。同一次
`epoll_wait` 结果可能包含多个事件位，所以一次分发也可能执行多个回调，而不是只能四选一。

### 1.3 Channel 在 epoll 中的三态

`Channel::index_` 在这里不是数组下标，而是 epoll 注册状态：

| 状态 | 值 | 含义 |
|---|---:|---|
| `kNew` | -1 | 从未加入，或已经彻底从 Poller 移除 |
| `kAdded` | 1 | 当前已注册到 epoll |
| `kDeleted` | 2 | 仍在 `channels_` 映射中，但暂时没有关注事件 |

状态转换（`src/EPollPoller.cc:55`）：

~~~text
kNew ──有关注事件/EPOLL_CTL_ADD──> kAdded
  ▲                                  │
  │                                  │ disableAll/EPOLL_CTL_DEL
  │                                  ▼
  └────removeChannel────────────── kDeleted
                         有新关注事件 │
                           ADD        └────> kAdded
~~~

`channels_` 保存 `fd → Channel*`，用于记录 Poller 当前认识的 Channel；
`epoll_event.data.ptr` 也直接保存 `Channel*`，使 `epoll_wait` 返回时不必再通过 fd 查表。
这两个位置都只是非拥有型裸指针，Channel 的所有者必须保证注册期间对象仍然存活。

### 1.4 为什么每个 EventLoop 都需要 eventfd

`epoll_wait` 最长可能阻塞 10 秒。main loop 把新连接交给 sub loop 时，不能等 sub loop
自然超时后才注册 connfd，因此每个 loop 构造时都会创建：

~~~text
wakeupFd_ (eventfd)
    ↕
wakeupChannel_（始终关注 EPOLLIN）
    ↕
本 EventLoop 的 epoll
~~~

跨线程调用链如下：

~~~text
线程 A：loopB->runInLoop(cb)
  → 发现自己不是 loopB 的线程
  → queueInLoop(cb)
  → 加锁放入 loopB.pendingFunctors_
  → wakeup() 向 loopB.wakeupFd_ 写 8 字节

线程 B：epoll_wait 返回 wakeupChannel_
  → EventLoop::handleRead() 读走 8 字节
  → 本轮 I/O Channel 分发结束
  → doPendingFunctors() 执行 cb
~~~

`doPendingFunctors()` 先在锁内用 `swap` 把共享队列换到局部变量，再在锁外执行回调。
这样既缩短临界区，也允许回调内部再次调用 `queueInLoop()`，不会因持有同一把锁而死锁。

`queueInLoop()` 在下面两种情况下唤醒 loop：

1. 调用者不是目标 loop 的线程——目标线程可能阻塞在 `epoll_wait`。
2. 当前正在执行 pending functor——新任务未进入本轮已 swap 出来的局部列表；唤醒可避免下一次 `epoll_wait` 再阻塞 10 秒。

### 1.5 EventLoop 构造后为什么已经有一个 fd

每个 `EventLoop` 构造时就把 `wakeupChannel_` 注册进自己的 Poller，所以还没有客户端时，
每个 sub loop 的日志也会显示 `fd total count:1`。main loop 开始监听后还多一个
`acceptChannel_`，因此显示 `fd total count:2`。这正好可以用运行日志验证对象关系，而不是只背结构图。

### 1.6 本阶段源码阅读断点

按下面的断点单步，比从头顺序阅读所有文件更容易建立闭环：

1. `src/EventLoop.cc:44`：构造 Poller、eventfd 和 wakeupChannel。
2. `src/Channel.cc:39`：从 Channel 更新一路进入 epoll。
3. `src/EPollPoller.cc:55`：观察 ADD/MOD/DEL 三态。
4. `src/EventLoop.cc:77`：观察一次完整事件循环。
5. `src/EPollPoller.cc:97`：把 `epoll_event.data.ptr` 还原为 Channel。
6. `src/Channel.cc:73`：从 `revents_` 分派四类回调。
7. `src/EventLoop.cc:126`：观察跨线程提交与 eventfd 唤醒。

完成这一节后，应该能不看代码回答：

- 为什么 `Channel` 同时需要 `events_` 和 `revents_`？
- 为什么修改 `Channel` 最终必须回到它所属的 loop 线程？
- `disableAll()` 与 `remove()` 为什么不是同一件事？
- main loop 如何让正阻塞在 `epoll_wait` 的 sub loop 立刻工作？
- 为什么执行 pending functor 时不能一直持有 `mutex_`？

## 2. 一条 TCP 连接的完整生命周期

不要把“接受连接、保存连接、处理连接 I/O”混成同一件事。这个仓库把它们分给两个线程域：

| 阶段 | 执行线程 | 主要对象 |
|---|---|---|
| 监听、`accept4`、创建连接、保存/删除连接表项 | main loop 线程 | `Acceptor`、`TcpServer` |
| connfd 注册、收发数据、关闭 Channel | 选中的 sub loop 线程 | `TcpConnection` |

线程池不是把一条连接的每次请求随意分给不同线程。**分配单位是一整条 TCP 连接**：
连接建立时选定一个 `ioLoop`，此后该连接的读写和 Channel 操作始终回到这个 loop。

### 2.1 启动：先创建 sub loop，再监听

示例入口是 `example/testserver.cc:54`：

~~~text
main
  → 创建 EventLoop mainLoop
  → 创建 EchoServer/TcpServer
      → 创建并 bind Acceptor
      → 创建 EventLoopThreadPool
  → 注册用户 connection/message callback
  → setThreadNum(3)
  → TcpServer::start
      → EventLoopThreadPool::start：创建 3 个线程及 3 个 sub loop
      → mainLoop.runInLoop(Acceptor::listen)
  → mainLoop.loop()
~~~

此时 `mainLoop.runInLoop()` 是从 main loop 自己的线程调用，所以 `Acceptor::listen()`
立即执行：底层 socket 开始监听，`acceptChannel_` 开启读事件。随后 `mainLoop.loop()`
进入 `epoll_wait`。

`TcpServer::start()` 用 `started_.fetch_add(1)` 保证只有第一次调用会真正启动线程池和监听。

### 2.2 建连：main loop 创建对象，sub loop 注册 Channel

listenfd 可读后的真实调用链（`src/Acceptor.cc:54`、`src/TcpServer.cc:67`）：

~~~text
main loop: epoll_wait 返回 acceptChannel
  → Channel::handleEvent
  → Acceptor::handleRead
  → accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC)
  → TcpServer::newConnection(connfd, peerAddr)
      → threadPool_->getNextLoop() 轮询选择 ioLoop
      → 创建 shared_ptr<TcpConnection>
      → 放入 TcpServer::connections_
      → 复制用户回调到 TcpConnection
      → 设置 closeCallback = TcpServer::removeConnection
      → ioLoop->runInLoop(TcpConnection::connectEstablished)
~~~

这里最容易误读的一点是：`TcpConnection` 对象由 main loop 线程创建，但其 `Channel`
构造时已经绑定目标 `ioLoop`，而且构造阶段还没有调用 `enableReading()`，所以尚未接触
目标 Poller。`connectEstablished()` 被投递到 sub loop 后才完成：

1. 状态从 `kConnecting` 改为 `kConnected`。
2. `channel_->tie(shared_from_this())` 建立生命周期保护。
3. `channel_->enableReading()`，在 sub loop 的 epoll 中注册 connfd。
4. 调用用户的 `connectionCallback_`，示例输出 `Connection UP`。

这就是“对象可以先在 main 线程创建，但所有 Channel/Poller 变更必须在所属 I/O 线程发生”。

### 2.3 两层回调：框架回调与用户回调

`TcpConnection` 构造时先把 Channel 的四个底层回调绑定到自己：

| Channel 事件 | 框架内部回调 |
|---|---|
| 可读 | `TcpConnection::handleRead` |
| 可写 | `TcpConnection::handleWrite` |
| 挂起/关闭 | `TcpConnection::handleClose` |
| 错误 | `TcpConnection::handleError` |

用户则把业务回调注册给 `TcpServer`，`TcpServer::newConnection()` 再复制给每个
`TcpConnection`。因此调用关系是：

~~~text
epoll 事件
  → Channel 框架回调
  → TcpConnection::handleXxx
  → 用户回调（EchoServer::onConnection/onMessage）
~~~

`Channel` 不认识 `TcpConnection`，`TcpConnection` 也不认识 `EchoServer`；它们通过
`std::function` 解耦。这是阅读 muduo 时比记类名更重要的设计。

### 2.4 收消息：数据先进入 inputBuffer_

connfd 可读时（`src/TcpConnection.cc:183`）：

~~~text
sub loop: epoll_wait
  → Channel::readCallback_
  → TcpConnection::handleRead(receiveTime)
  → inputBuffer_.readFd(connfd)
      ├── n > 0：messageCallback_(conn, &inputBuffer_, time)
      ├── n = 0：对端正常关闭，handleClose()
      └── n < 0：保存 errno，handleError()
~~~

示例的 `EchoServer::onMessage()` 调用 `retrieveAllAsString()` 消费输入缓冲区，然后
`conn->send(msg)` 原样发回。这里的“消息”只是本次 Buffer 中已有的字节；TCP 本身没有
消息边界，生产协议仍需自行处理半包、粘包和长度字段。

### 2.5 发消息：先直接写，写不完才监听 EPOLLOUT

`TcpConnection::sendInLoop()` 的策略是：

1. 如果当前没有待发送数据，也没有监听写事件，先直接 `write(connfd)`。
2. 一次写完：把 `writeCompleteCallback_` 排入 loop，无需关注 `EPOLLOUT`。
3. 只写了一部分或返回 `EWOULDBLOCK`：把剩余字节追加到 `outputBuffer_`，再
   `enableWriting()`。
4. 后续 socket 可写时，`handleWrite()` 从 `outputBuffer_` 继续发送。
5. 缓冲区清空后立即 `disableWriting()`；如果正在 shutdown，再执行半关闭。

不能从连接建立起就永久监听 `EPOLLOUT`：正常 socket 大部分时间都是可写的，在 LT
模式下会让 `epoll_wait` 不断立即返回，形成无意义的忙循环。

输出缓冲区从低于 64 MiB 跨越到不低于 64 MiB 时，可以触发高水位回调。这不是自动
限流，而是一个背压信号；业务层仍需决定暂停生产、断开慢连接或采取其他策略。

### 2.6 断连：为什么要在 main 和 sub loop 之间往返

客户端关闭连接后，sub loop 中 `readv()` 返回 0，进入 `handleClose()`：

~~~text
sub loop
  TcpConnection::handleClose
    → state = kDisconnected
    → channel.disableAll()              // epoll DEL，状态变 kDeleted
    → 用户 connectionCallback(DOWN)
    → closeCallback(conn)
        → TcpServer::removeConnection

main loop
  TcpServer::removeConnectionInLoop
    → connections_.erase(conn->name()) // 连接表只在 main loop 修改
    → ioLoop->queueInLoop(connectDestroyed)

原 sub loop
  TcpConnection::connectDestroyed
    → channel.remove()                  // 从 Poller 映射删除，状态回 kNew
    → 最后的 shared_ptr 释放
    → TcpConnection 析构
    → Socket 析构并 close(connfd)
~~~

往返的原因有两个：

- `connections_` 归 main loop 管，避免多个 I/O 线程并发修改连接表。
- `Channel` 归原 `ioLoop` 管，必须回到原线程移除。

这里特意使用 `queueInLoop(connectDestroyed)`，即使单线程模式下目标就是当前 loop，也会
延迟到当前 Channel 事件处理完成后再销毁，避免在自己的回调栈中删除自己。

### 2.7 shared_ptr、weak_ptr 与 tie

连接存活时的主要所有者是 `TcpServer::connections_` 中的 `shared_ptr<TcpConnection>`。
异步投递的回调也会临时捕获一份 `shared_ptr`，保证任务执行前对象不会消失。

Channel 内部不能再长期保存一份 `shared_ptr`，否则会形成类似
`TcpConnection → Channel → TcpConnection` 的引用环。因此 `Channel::tie()` 保存
`weak_ptr<void>`。开始处理事件时临时 `lock()` 成 `shared_ptr guard`：

- lock 成功：对象至少活到本次 `handleEvent()` 结束。
- lock 失败：对象已经销毁，不再调用绑定了裸 `this` 的回调。

这解释了为什么 `tie()` 既能保护回调生命周期，又不会阻止连接最终析构。

### 2.8 当前实现需要警惕的边界

理解设计时也要区分“muduo 思路”和“这份练习代码的实现质量”：

1. `TcpConnection::send()` 的跨线程分支把 `buf.c_str()` 和裸 `this` 放进延迟回调；
   原字符串或连接可能在执行前失效。示例的 `onMessage()` 本就在连接所属线程，所以
   6 次 echo 验证没有覆盖这个问题。
2. `Acceptor` 构造函数接收 `reuseport`，但实现始终调用 `setReusePort(true)`，参数没有生效。
3. 多个线程直接写 `std::cout`，运行时日志会交错；日志文本不能被当作严格时序记录。
4. 用户的 connection/message callback 在若干路径中被直接调用，未设置时可能抛出
   `std::bad_function_call`；当前示例两者都已设置。

这些问题适合在理解主线后单独修复和加回归测试，不应反过来干扰 Reactor 模型的学习。

完成本节后，应该能不看代码回答：

- 为什么 `TcpConnection` 在 main loop 创建，却在 sub loop 注册 connfd？
- 为什么一条连接不能在三个 sub loop 之间逐请求轮转？
- 为什么发送数据不能永久监听 `EPOLLOUT`？
- `disableAll()`、`connections_.erase()`、`channel.remove()`、`close(fd)` 分别在哪一步？
- `Channel::tie()` 为什么使用 weak_ptr，而不是直接保存 shared_ptr？

## 3. one loop per thread 到底怎样落地

### 3.1 三层封装不要混淆

线程相关代码分为三层：

| 层 | 类 | 作用 |
|---|---|---|
| 操作系统线程包装 | `Thread` | 启动/join `std::thread`，记录 Linux tid 和线程名 |
| 一个线程配一个循环 | `EventLoopThread` | 在线程栈上创建 `EventLoop`，协调调用者拿到其地址 |
| 多个 I/O 循环的集合 | `EventLoopThreadPool` | 创建多个 `EventLoopThread`，保存 loop 地址并轮询选择 |

`EventLoopThreadPool` 并不是通用任务线程池：它没有共享工作队列，也不会让任意线程抢任务。
它管理的是多个长期运行的 Reactor，每个 Reactor 各自有 epoll、Channel 集合和任务队列。

### 3.2 创建一个 sub loop 时有两次握手

第一次握手发生在 `Thread::start()`（`src/Thread.cc:23`）：

~~~text
调用线程                         新 std::thread
   │                                  │
   ├── sem_init(0)                    │
   ├── 创建线程 ─────────────────────>│ CurrentThread::tid()
   ├── sem_wait()                     │ 写入 Thread::tid_
   │<─────────────────────────────────┤ sem_post()
   └── start() 返回                   │ func_()
~~~

这保证 `Thread::start()` 返回时，新线程的 Linux tid 已经可用。

第二次握手发生在 `EventLoopThread::startLoop()`（`src/EventLoopThread.cc:31`）：

~~~text
main loop 线程                    新 I/O 线程
   │                                  │
   ├── thread_.start() ──────────────>│ EventLoop loop（在线程栈上）
   ├── cond_.wait(loop_ != nullptr)   │ 执行 ThreadInitCallback
   │                                  │ loop_ = &loop
   │<─────────────────────────────────┤ cond_.notify_one()
   ├── 得到 EventLoop*                │ loop.loop()
   └── 保存到线程池 loops_            │
~~~

第一个信号量同步“线程 tid 已就绪”，第二个条件变量同步“线程内的 EventLoop 已构造完成”。
两者保护的是不同初始化阶段。

`EventLoop` 是新线程 `threadFunc()` 的栈变量，所以 `loops_` 中保存的只是非拥有型指针。
真正控制其生命周期的是 `EventLoopThread`：析构时调用 `loop_->quit()`，跨线程写 eventfd
唤醒阻塞的 loop，再 `join()`，等 `threadFunc()` 返回后栈上 EventLoop 才析构。

### 3.3 一个线程为何最多有一个 EventLoop

`src/EventLoop.cc:13` 定义了线程局部变量：

~~~cpp
__thread EventLoop *t_loopInThisThread = nullptr;
~~~

构造 `EventLoop` 时，如果本线程已有非空指针就记录致命错误；否则登记当前对象。析构时
再置回空指针。这条约束让“当前线程”和“当前 Reactor”形成一一对应关系，也是
`isInLoopThread()` 可以依赖创建时 `threadId_` 的基础。

这里缓存的是 Linux tid，而不是 `std::thread::id`。`CurrentThread::tid()` 第一次通过
`SYS_gettid` 获取，再放入 `__thread t_cachedTid`，后续判断线程归属无需重复系统调用。

### 3.4 setThreadNum(3) 实际上一共有四个 loop

当前示例在 `example/testserver.cc:21` 调用 `setThreadNum(3)`，含义是创建三个额外的
sub loop；用户在 main 中创建的 base/main loop 不包含在这个数字里：

~~~text
1 个 main loop（accept + 连接表管理）
+
3 个 sub loop（已连接 socket 的 I/O）
=
4 个 EventLoop / 4 个线程
~~~

真正的单线程模式是 `setThreadNum(0)` 或不设置线程数，此时 `loops_` 为空，
`getNextLoop()` 总是返回 `baseLoop_`。如果设置为 1，则是 main loop 加一个 sub loop，
不是“总共只有 main loop”。源码 `getNextLoop()` 附近有一条注释把这一点写反了，阅读时
应以 `for (i < numThreads_)` 和 `loops_.empty()` 的实际分支为准。

### 3.5 round-robin 是连接级粘性分配

线程池的选择逻辑只有一个 `next_`：

~~~text
连接 #1 → loops_[0]
连接 #2 → loops_[1]
连接 #3 → loops_[2]
连接 #4 → loops_[0]
...
~~~

选定之后，`TcpConnection::loop_` 永远指向该 loop，连接不会迁移。这样做的主要价值不是
找到“当前最空闲”的线程，而是让同一连接的状态、Channel 和 Buffer 保持线程亲和性。
代价是长连接负载差异很大时，简单轮询不一定均衡。

### 3.6 哪些数据跨线程，哪些数据不跨线程

| 数据/操作 | 线程规则 | 同步方式 |
|---|---|---|
| `TcpServer::connections_` | 只在 main loop 读写 | 通过 `mainLoop.runInLoop()` 汇合 |
| `TcpConnection` 状态、Channel、Buffer | 只在所属 ioLoop 操作 | 通过 `ioLoop.runInLoop/queueInLoop()` 汇合 |
| `EventLoop::pendingFunctors_` | 多线程可提交，所属线程执行 | `mutex_` + eventfd |
| `EventLoop::quit_`、`TcpServer::started_` 等标志 | 可能跨线程 | 原子变量 |
| `Poller::channels_` | 只应由所属 loop 线程访问 | 依靠线程亲和约定 |

因此 muduo 的线程安全思想不是“给所有对象都加锁”，而是：

> 让可变状态固定归一个线程；跨线程只传递任务，任务回到所有者线程后再修改状态。

当前精简实现没有原版常见的 `assertInLoopThread()` 防御检查，`updateChannel()` 等接口也
是公开的，所以错误调用不会被尽早拦截。学习和扩展时仍必须遵守线程亲和约定。

完成本节后，应该能不看代码回答：

- `Thread::start()` 与 `EventLoopThread::startLoop()` 为什么各需要一次同步？
- 为什么 `loops_` 保存裸指针仍可以工作，其真实所有者是谁？
- `setThreadNum(0/1/3)` 分别会产生多少个 EventLoop？
- 为什么线程池按连接分配，而不是按消息分配？
- 为什么 `pendingFunctors_` 需要锁，而连接自己的 Buffer 通常不需要锁？

## 4. Buffer、非阻塞 I/O 与背压

### 4.1 Buffer 的三个区域与一个不变量

`Buffer` 用一个 `std::vector<char>` 和两个下标表示三个逻辑区域：

~~~text
0                 readerIndex_            writerIndex_          buffer_.size()
│                      │                       │                       │
├──── prependable ─────┼────── readable ──────┼────── writable ──────┤
│      已消费/预留      │      等待业务读取      │      可继续写入        │
~~~

任何正常状态都满足：

~~~text
kCheapPrepend <= readerIndex_ <= writerIndex_ <= buffer_.size()
readableBytes()    = writerIndex_ - readerIndex_
writableBytes()    = buffer_.size() - writerIndex_
prependableBytes() = readerIndex_
~~~

初始 vector 大小是 `8 + 1024` 字节，读写下标都从 8 开始。前 8 字节是 muduo
常见的头部预留思想，不过当前精简类没有实现 `prepend()`，所以它主要参与空间整理。

`append()` 推进 `writerIndex_`；`retrieve(len)` 推进 `readerIndex_`；全部消费后
`retrieveAll()` 把两个下标重置为 8，而不是释放 vector 内存。这样下一条数据通常可以
复用已有容量。

### 4.2 空间不足时：先搬移，实在不够再扩容

`ensureWritableBytes(len)` 发现尾部空间不足后进入 `makeSpace(len)`：

~~~text
情况 A：尾部 writable + 前部已消费空间仍然不够
  → resize(writerIndex_ + len)

情况 B：两部分合起来够用
  → 把 readable 数据搬到 kCheapPrepend 之后
  → 重设 readerIndex_ 和 writerIndex_
  → 不扩容
~~~

所以 `prependableBytes()` 不只是浪费掉的空间；业务消费数据后，前面的空间可以通过一次
内存搬移重新变成连续可写区。权衡是：搬移节省分配，但仍有 `O(readableBytes)` 的复制。

### 4.3 为什么读 socket 使用 readv + 64 KiB 栈缓冲区

收到可读事件时，我们不知道内核里当前有多少字节，而 vector 尾部空间可能很小。
`Buffer::readFd()` 构造两个 `iovec`：

~~~text
iovec[0] → Buffer 当前 writable 区域
iovec[1] → 栈上的 extrabuf[65536]
~~~

然后只调用一次 `readv()`：

- 数据不超过 vector 尾部：直接推进 `writerIndex_`，没有额外复制。
- 数据溢出到 `extrabuf`：先把 vector 视为写满，再 `append()` 溢出的那部分并按需扩容。
- vector 尾部本来就大于等于 64 KiB：只传一个 iovec，不使用额外缓冲区。

这样既避免每次读取前都盲目把 vector 扩到很大，又尽量用一次系统调用读入更多数据。
当前 epoll 没有设置 `EPOLLET`，工作在 LT 模式；一次 `readv()` 没读完也没关系，只要
socket 仍可读，下一轮 `epoll_wait` 还会报告该 fd。

### 4.4 TCP 字节流不等于业务消息

`handleRead()` 每次把当时可读的数据追加进同一个 `inputBuffer_`，但一次读取可能包含：

- 半条业务消息；
- 正好一条消息；
- 多条消息粘在一起。

当前 echo 示例直接 `retrieveAllAsString()`，因为它只需要把收到的所有字节原样返回，
并没有定义业务协议。真实协议的 `onMessage()` 应循环检查 Buffer，例如先判断固定长度头，
再根据头部长度判断完整包体是否到齐；不完整就保留在 Buffer，等待下次读事件继续追加。

### 4.5 输出 Buffer 是用户态的“待发送队列”

非阻塞 `write()` 只保证调用不会长时间阻塞，不保证一次写完。内核发送缓冲区满时，未发送
部分必须留在 `outputBuffer_`，否则数据会丢失：

~~~text
业务 send(data)
  │
  ├── write 全部成功 ────────────────> 完成
  │
  └── write 部分成功/EWOULDBLOCK
        → 剩余数据 append 到 outputBuffer_
        → Channel 开启 EPOLLOUT
        → socket 再次可写
        → handleWrite/writeFd
        → retrieve(已写字节)
        ├── 仍有剩余：继续等 EPOLLOUT
        └── 已清空：关闭 EPOLLOUT，触发完成回调
~~~

`inputBuffer_` 与 `outputBuffer_` 都属于某一个 `TcpConnection`，正常情况下只在该连接的
ioLoop 线程访问，因此 Buffer 自身不需要 mutex。

### 4.6 高水位只报警，不自动解决慢连接

当 `outputBuffer_` 从低于 `highWaterMark_` 跨越阈值时，代码把
`highWaterMarkCallback_` 排入 loop。默认阈值是 64 MiB。

注意它只在“向上跨越阈值”的瞬间回调，不会在阈值以上每次 append 都回调；当前实现也
没有低水位回调。这个信号说明业务生产速度持续高于网络发送速度，常见处理包括：

- 暂停从上游继续读取或生成数据；
- 限制单连接待发送字节数；
- 对长期慢连接超时或断开；
- 等输出降到安全范围后恢复生产。

如果业务忽略高水位，网络库仍会继续扩展 vector，慢客户端可能导致进程内存不断增长。

### 4.7 shutdown 是半关闭，不是立刻 close

`TcpConnection::shutdown()` 先把状态改成 `kDisconnecting`。如果当前没有待发送数据，
马上调用 `shutdown(fd, SHUT_WR)`；如果仍在监听 EPOLLOUT，则等 `handleWrite()` 清空
`outputBuffer_` 后再半关闭写端。

这保证已经接受的业务数据尽量发送完。真正的 `close(fd)` 仍发生在连接从服务器表和
Poller 移除、最后一个 `shared_ptr` 释放、`Socket` 析构的时候。

### 4.8 Buffer/发送路径的实现边界

1. `retrieveAsString(len)` 没有检查 `len <= readableBytes()`；当前调用点传入的是安全值，
   新增协议解析代码时需要自己先验证长度。
2. `write()` 没有使用 `MSG_NOSIGNAL`，代码也未见全局忽略 `SIGPIPE`；对已经关闭的连接
   写数据时，进程可能在检查 `EPIPE` 之前先收到信号。生产实现需要明确处理。
3. `sendFileInLoop()` 没把剩余文件数据纳入 EPOLLOUT 状态机，而是不断 `queueInLoop()`
   重试；遇到持续 `EWOULDBLOCK` 时可能忙循环，且调用方还必须保证文件 fd 一直有效。
4. 当前压力脚本按“发送字符串长度”精确接收，能验证 echo 字节完整性，但不能替代针对
   Buffer 扩容、搬移、半包、部分写和高水位行为的单元测试。

完成本节后，应该能不看代码回答：

- `readerIndex_` 与 `writerIndex_` 各自怎样移动？
- 尾部不够时，什么情况搬移、什么情况扩容？
- `readv + extrabuf` 怎样兼顾少分配和少系统调用？
- 为什么一次 `readFd()` 不能对应一条业务消息？
- 为什么只在有积压时监听 EPOLLOUT，清空后必须关闭？
- 高水位回调和真正的流量控制有什么区别？

---

## 5. 构建与运行验证

### 5.1 环境依赖

确保系统已安装以下基础依赖：
CMake（建议 3.10 及以上版本）
GCC/G++（支持 C++11 及以上标准）
make 工具
基础的 Linux 开发环境（如 libc6-dev 等）

在项目根目录执行：

~~~bash
cmake -S . -B build
cmake --build build --parallel
./example/testserver
~~~

构建会生成 `lib/libmuduo.so` 和 `example/testserver`。服务默认监听
`127.0.0.1:8080`，示例配置三个 sub loop。

### 5.2 压测脚本

实际脚本是 `example/tcp_echo_bench.py`。下面保留其实现，便于理解它如何通过
“按发送长度精确读取”处理客户端接收侧的半包；日常运行应直接执行脚本文件。
~~~
import socket
import threading
import time
import argparse
from datetime import datetime
from collections import defaultdict

# 全局统计变量
stats = {
    "total_requests": 0,    # 总请求数
    "success_requests": 0,  # 成功请求数
    "failed_requests": 0,   # 失败请求数
    "total_latency": 0.0,   # 总延迟（秒）
    "max_latency": 0.0,     # 最大延迟（秒）
    "min_latency": float('inf'),  # 最小延迟（秒）
    "lock": threading.Lock()  # 线程安全锁
}


def recv_exact(sock, expected_len):
    """
    从 socket 中精确读取 expected_len 字节，直到读满或出错/超时。
    返回 bytes；如果中途失败则抛异常。
    """
    chunks = []
    bytes_recd = 0
    while bytes_recd < expected_len:
        chunk = sock.recv(expected_len - bytes_recd)
        if not chunk:
            # 连接被对端关闭
            raise ConnectionError("socket closed before receiving expected data")
        chunks.append(chunk)
        bytes_recd += len(chunk)
    return b"".join(chunks)


def client_worker(server_ip, server_port, msg, msg_count, timeout=5):
    """
    单个客户端线程逻辑：连接服务器，发送指定数量的消息并接收回显
    :param server_ip: 服务器IP
    :param server_port: 服务器端口
    :param msg: 要发送的消息内容
    :param msg_count: 每个客户端发送的消息数
    :param timeout: 连接/读写超时时间（秒）
    """
    global stats
    sock = None
    try:
        # 创建TCP连接
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(timeout)  # 设置超时
        # 连接服务器
        sock.connect((server_ip, server_port))
        
        for _ in range(msg_count):
            start_time = time.time()
            try:
                # 发送消息
                sock.sendall(msg.encode('utf-8'))
                # 接收回显（按消息长度接收，避免粘包 / 半包）
                expected_len = len(msg.encode('utf-8'))
                recv_data = recv_exact(sock, expected_len)
                
                # 统计结果
                latency = time.time() - start_time
                with stats["lock"]:
                    stats["total_requests"] += 1
                    if recv_data.decode('utf-8') == msg:
                        stats["success_requests"] += 1
                        stats["total_latency"] += latency
                        stats["max_latency"] = max(stats["max_latency"], latency)
                        stats["min_latency"] = min(stats["min_latency"], latency)
                    else:
                        stats["failed_requests"] += 1
            except Exception as e:
                with stats["lock"]:
                    stats["total_requests"] += 1
                    stats["failed_requests"] += 1
                # print(f"发送/接收失败: {e}")
    except Exception as e:
        with stats["lock"]:
            # 连接失败则所有请求算失败
            stats["total_requests"] += msg_count
            stats["failed_requests"] += msg_count
        # print(f"客户端连接失败: {e}")
    finally:
        if sock:
            sock.close()

def print_stats(start_time, end_time):
    """打印压测统计结果"""
    total_time = end_time - start_time
    qps = stats["success_requests"] / total_time if total_time > 0 else 0
    avg_latency = stats["total_latency"] / stats["success_requests"] if stats["success_requests"] > 0 else 0
    
    print("\n" + "="*50)
    print(f"压测结束 | 时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print("="*50)
    print(f"总耗时: {total_time:.2f} 秒")
    print(f"总请求数: {stats['total_requests']}")
    print(f"成功请求数: {stats['success_requests']}")
    print(f"失败请求数: {stats['failed_requests']}")
    print(f"成功率: {stats['success_requests']/stats['total_requests']*100:.2f}%" if stats['total_requests'] > 0 else "成功率: 0%")
    print(f"QPS (每秒成功请求数): {qps:.2f}")
    print(f"平均延迟: {avg_latency*1000:.2f} 毫秒")
    print(f"最大延迟: {stats['max_latency']*1000:.2f} 毫秒")
    print(f"最小延迟: {stats['min_latency']*1000:.2f} 毫秒" if stats['min_latency'] != float('inf') else "最小延迟: 0 毫秒")
    print("="*50)

def main():
    # 命令行参数解析
    parser = argparse.ArgumentParser(description='TCP Echo 服务器压力测试工具')
    parser.add_argument('--ip', default='127.0.0.1', help='服务器IP地址，默认127.0.0.1')
    parser.add_argument('--port', type=int, required=True, help='服务器端口（必填）')
    parser.add_argument('--concurrency', type=int, default=100, help='并发客户端数，默认100')
    parser.add_argument('--msgs-per-client', type=int, default=1000, help='每个客户端发送的消息数，默认1000')
    parser.add_argument('--msg', default='hello, muduo!\n', help='发送的消息内容，默认"hello, muduo!\n"')
    parser.add_argument('--timeout', type=int, default=5, help='连接/读写超时时间（秒），默认5')
    
    args = parser.parse_args()
    
    # 打印压测配置
    print("="*50)
    print(f"压测开始 | 时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print("="*50)
    print(f"服务器地址: {args.ip}:{args.port}")
    print(f"并发客户端数: {args.concurrency}")
    print(f"每个客户端发送消息数: {args.msgs_per_client}")
    print(f"单条消息内容: {repr(args.msg)}")
    print(f"超时时间: {args.timeout} 秒")
    print("="*50)
    
    # 记录开始时间
    start_time = time.time()
    
    # 创建并发客户端线程
    threads = []
    for i in range(args.concurrency):
        t = threading.Thread(
            target=client_worker,
            args=(args.ip, args.port, args.msg, args.msgs_per_client, args.timeout)
        )
        threads.append(t)
        t.start()
    
    # 等待所有线程结束
    for t in threads:
        t.join()
    
    # 记录结束时间并打印统计
    end_time = time.time()
    print_stats(start_time, end_time)

if __name__ == '__main__':
    main()
~~~

#### 示例 1：2000 并发 × 500 条/客户端 = 100 万请求

python3 tcp_echo_bench.py --port 8080 --concurrency 2000 --msgs-per-client 500

#### 示例 2：1000 并发 × 1000 条/客户端 = 100 万请求

python3 tcp_echo_bench.py --port 8080 --concurrency 1000 --msgs-per-client 1000

脚本采用“一客户端一 Python 线程”，不要一开始就用 1000/2000 并发判断网络库上限；
客户端自身的线程数、文件描述符限制和大量 INFO 日志都会先影响结果。先使用小规模命令
验证正确性，再逐步增加并发：

~~~bash
python3 example/tcp_echo_bench.py \
  --port 8080 --concurrency 3 --msgs-per-client 2 --msg ping --timeout 2
~~~

## 6. 全库源码地图与推荐阅读路线

### 6.1 模块地图

| 模块 | 文件 | 阅读目标 |
|---|---|---|
| 基础语义 | `noncopyable.h` | 为什么资源对象禁止复制 |
| 时间与日志 | `Timestamp.*`、`Logger.*` | 日志辅助；不要把它当网络主线 |
| 线程标识 | `CurrentThread.*` | Linux tid 的线程局部缓存 |
| 线程封装 | `Thread.*` | `std::thread` 启动与 tid 握手 |
| 地址与 fd | `InetAddress.*`、`Socket.*` | sockaddr 转换与 fd 的 RAII 生命周期 |
| 缓冲区 | `Buffer.*` | 下标、readv、部分写 |
| 事件描述 | `Channel.*` | events/revents 与回调分发 |
| I/O 多路复用 | `Poller.*`、`DefaultPoller.cc`、`EPollPoller.*` | epoll 抽象与 ADD/MOD/DEL |
| Reactor | `EventLoop.*` | poll、Channel 分发、跨线程任务与 eventfd |
| I/O 线程 | `EventLoopThread.*`、`EventLoopThreadPool.*` | one loop per thread 与连接轮询 |
| 回调类型 | `Callbacks.h` | shared_ptr 连接句柄和业务回调签名 |
| 接受连接 | `Acceptor.*` | listenfd 的 Channel 与 accept4 |
| 连接对象 | `TcpConnection.*` | 状态机、收发、Buffer、关闭与 tie |
| 服务对象 | `TcpServer.*` | 启动、连接表、跨 loop 创建/销毁 |
| 最小应用 | `example/testserver.cc` | 用户怎样组装上述接口 |

### 6.2 不要按文件名字母顺序读

推荐分五轮，每一轮只追一条闭环：

1. **从使用面进入**：`testserver.cc → TcpServer.h → Callbacks.h`，先知道用户能做什么。
2. **Reactor 最小闭环**：`EventLoop → EPollPoller → Channel → EventLoop`。
3. **建连闭环**：`Acceptor::handleRead → TcpServer::newConnection → connectEstablished`。
4. **收发闭环**：`handleRead → Buffer → onMessage → sendInLoop → handleWrite`。
5. **关闭闭环**：`handleClose → removeConnectionInLoop → connectDestroyed → Socket::~Socket`。

辅助类应在调用链遇到时再读，不必先花大量时间背 `Logger`、`Timestamp` 或所有 socket
选项。每读完一轮，用本节列出的五个问题脱离源码复述；无法复述的节点再回到对应断点。

### 6.3 四条适合调试器跟踪的路径

~~~text
启动：main → TcpServer::start → EventLoopThreadPool::start
      → EventLoopThread::threadFunc → EventLoop::loop

建连：EPollPoller::poll → Channel::handleEvent → Acceptor::handleRead
      → TcpServer::newConnection → TcpConnection::connectEstablished

回显：TcpConnection::handleRead → Buffer::readFd → EchoServer::onMessage
      → TcpConnection::send → sendInLoop

断连：TcpConnection::handleClose → TcpServer::removeConnection
      → removeConnectionInLoop → connectDestroyed → TcpConnection::~TcpConnection
~~~

调试时建议同时观察：当前 Linux tid、`EventLoop*`、fd、`Channel::index_`、
`events_/revents_`、连接状态，以及 Buffer 的 reader/writer 下标。这样能同时看见
“在哪个线程、哪个 loop、哪个 fd、哪个生命周期阶段”。

## 7. 当前实现与生产级 muduo 的差距

下面是源码核对后确认的边界，理解这个仓库时应主动记账：

| 位置 | 当前行为 | 影响 |
|---|---|---|
| `DefaultPoller.cc` | 设置 `MUDUO_USE_POLL` 后返回 nullptr，并没有 poll 实现 | 后续解引用会崩溃；只能使用 epoll |
| `Timestamp.cc` | 字段名写“微秒”，实际 `time(NULL)` 只有秒级 | poll 返回时间并不精确到微秒 |
| `Logger` | 多线程共享可变 `logLevel_`，没有同步 | 存在数据竞争，级别和文本可能交错 |
| `EPollPoller/Channel` | 每轮 poll 和每个事件都打 INFO | 严重扰动性能测试和日志可读性 |
| `Acceptor` | 忽略 `reuseport` 参数，始终开启 SO_REUSEPORT | `TcpServer::Option` 没有真正生效 |
| `Acceptor::handleRead` | 一次就绪只 accept 一个连接 | LT 下仍能继续触发，但突发建连处理效率较低 |
| `TcpConnection::send` | 跨线程延迟回调捕获裸 `this` 与 `buf.c_str()` | 可能发生对象或字符串生命周期失效 |
| `TcpConnection::sendFileInLoop` | EWOULDBLOCK 后排队立即重试，未接入 EPOLLOUT | 可能忙循环，且文件 fd 生命周期由调用者承担 |
| 回调调用 | 若用户未设置 connection/message callback，部分路径直接调用空 function | 可能抛 `std::bad_function_call` |
| 线程约束 | 缺少 `assertInLoopThread()` 一类检查 | 错误的跨线程 Channel 操作难以及早发现 |
| 模块范围 | 没有 Timer、TcpClient、Connector 等 | 只能学习服务端 Reactor 核心，不代表完整 muduo |

另外，发送路径没有明确屏蔽 `SIGPIPE`；生产代码需要使用 `MSG_NOSIGNAL`、忽略信号或采用
其他一致策略。以上问题目前作为学习审计记录，没有擅自改变库行为。

## 8. 已验证基线与后续练习

### 8.1 已验证

- `cmake --build build --parallel 2`：`muduo` 与 `testserver` 均构建成功。
- 运行一个 main loop 和三个 sub loop：启动成功。
- 3 个并发客户端 × 每个 2 条 `ping`：总计 6 次，成功 6、失败 0。
- 日志观察：三个连接分别分配给三个 sub loop；每条连接都完成 UP、读写、DOWN、
  从 main loop 连接表删除、回所属 sub loop 移除 Channel、析构并关闭 fd。
- 文档修改通过 `git diff --check`。

这 6 次请求只证明最小 echo 功能和调用链可运行，不证明生产稳定性或性能上限。

### 8.2 建议按顺序完成的代码练习

1. 给 `Buffer` 增加单元测试：复位、搬移、扩容、分段解析。
2. 给 `EventLoop::runInLoop/queueInLoop` 写双线程测试，记录回调实际执行 tid。
3. 把 echo 协议改成“4 字节长度头 + 包体”，主动构造半包与粘包。
4. 修复 `TcpConnection::send` 的跨线程生命周期问题，并用另一个线程反复发送验证。
5. 降低热路径日志级别，再进行逐级并发测试；同时记录客户端和服务端资源限制。
6. 最后再实现 TimerQueue，理解 timerfd 与普通 Channel 如何统一进入 EventLoop。

这些练习的顺序对应本笔记的结构：先验证局部不变量，再验证跨线程调度，然后扩展协议和
修复生命周期，最后才做性能与新模块，避免一次同时面对过多变量。
