---
title: muduo网路库的学习

---



# 项目核心组件关系与数据流向图

核心框架：**Reactor 模型**（one loop per thread），核心流转是「事件触发→组件协作→数据处理」，所有组件围绕 “高效处理网络 IO” 联动。



## 核心组件关联总览

~~~
[客户端] ↔ [Socket] ↔ [Channel] ↔ [Poller] ↔ [EventLoop]
                          ↑
                  [Acceptor]（主循环专属）
                          ↑
[TcpServer] → [TcpConnection] ↔ [Buffer]（读写数据载体）
~~~

总结下来，流程就是：服务器启动（loop 启动） → Acceptor 监听端口 → 客户端连接触发读事件 → handleRead 接收 connfd 并交给 TcpConnection → Buffer 读写数据 → 业务层处理数据/返回响应。



# 项目前置工具实现细节

## noncopyable 类
C++ 中， 默认情况下所有的类都支持拷贝和构造， 有些对象逻辑上不应该被复制，
例如本项目的的 Eventloop（事件循环， 一个线程只能有一个），
Socket, Acceptor 复制会导致fd重复关闭， 所以需要这样的一个基类

noncopyable 核心作用是进制派生类对象被拷贝或赋值
通过删除拷贝构造函数和拷贝赋值运算符，确保继承它的类（派生类）无法被复制或赋值，
从而避免因对象拷贝导致的资源管理问题（如重复释放、资源不一致等）。

代码实现细节

**noncopyable.h**
~~~
#pragma once // 预处理指令，防止头文件重复包含

class noncopyable {
public:
    // 禁止这两类常见的拷贝操作
    noncopyable(const noncopyable &) = delete;
    noncopyable &operator = (const noncopyable &) = delete;

protected:
// 将构造函数声明为 protected（受保护的），意味着只有它的派生类（子类）才能访问。
// 这就防止了用户直接创建 noncopyable 
// 类的实例（因为在类外部无法调用 protected 的构造函数），符合其作为基类的设计初衷。
    noncopyable() = default;
    ~noncopyable() = default;
};
~~~

## 日志工具

为了 统一日志格式、简化日志输出、支持级别过滤和调试开关
我们需要一个日志工具

实现一个日志核心类 logger， 单例 + 不可复制

日志级别枚举
四种日志级别，区分日志重要程度
~~~
enum LogLevel {
    INFO,  // 普通信息（如程序启动、连接建立）
    ERROR, // 错误信息（如连接失败、读写超时，不影响程序运行）
    FATAL, // 致命错误（如绑定端口失败、内存分配失败，程序必须退出）
    DEBUG, // 调试信息（如变量值、函数调用流程，仅开发阶段启用）
};
~~~

日志输出宏定义

~~~
#define LOG_INFO(logmsgFormat, ...)                       \
    do                                                    \
    {                                                     \
        Logger &logger = Logger::instance();              \
        logger.setLogLevel(INFO);                         \
        char buf[1024] = {0};                             \
        snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__); \
        logger.log(buf);                                  \
    } while (0)
~~~

#define LOG_INFO(logmsgFormat, ...)：宏定义的参数列表；
logmsgFormat：日志格式化字符串（如 "bind sockfd:%d fail"）；
...：可变参数（对应格式化字符串中的占位符，如 %d 对应的 sockfd_）；
##__VA_ARGS__：C99 语法，将可变参数传递给 snprintf，## 确保 “无可变参数时也能编译通过”（避免语法错误）。

do-while(0) 包裹，避免 在 if-else 等语句中因缺少大括号导致逻辑错误

日志核心类的实现

**Logger.h**
~~~
#pragma once

#include <cstring>
#include "noncopyable.h"

enum LogLevel {
    INFO,  // 普通信息（如程序启动、连接建立）
    ERROR, // 错误信息（如连接失败、读写超时，不影响程序运行）
    FATAL, // 致命错误（如绑定端口失败、内存分配失败，程序必须退出）
    DEBUG, // 调试信息（如变量值、函数调用流程，仅开发阶段启用）
};

#define LOG_INFO(logmsgFormat, ...)                       \
    do                                                    \
    {                                                     \
        Logger &logger = Logger::instance();              \
        logger.setLogLevel(INFO);                         \
        char buf[1024] = {0};                             \
        snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__); \
        logger.log(buf);                                  \
    } while (0)

#define LOG_ERROR(logmsgFormat, ...)                       \
    do                                                    \
    {                                                     \
        Logger &logger = Logger::instance();              \
        logger.setLogLevel(ERROR);                         \
        char buf[1024] = {0};                             \
        snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__); \
        logger.log(buf);                                  \
    } while (0)

#define LOG_FATAL(logmsgFormat, ...)                       \
    do                                                    \
    {                                                     \
        Logger &logger = Logger::instance();              \
        logger.setLogLevel(FATAL);                         \
        char buf[1024] = {0};                             \
        snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__); \
        logger.log(buf);                                  \
    } while (0)



// 1. 编译开关判断：是否定义了 MUDEBUG 宏
#ifdef MUDEBUG
// 2. 若定义（调试模式）：定义 LOG_DEBUG 宏，执行日志输出逻辑
#define LOG_DEBUG(logmsgFormat, ...)                      \
    do                                                    \
    {                                                     \
        Logger &logger = Logger::instance();              \
        logger.setLogLevel(DEBUG);                        \
        char buf[1024] = {0};                             \
        snprintf(buf, 1024, logmsgFormat, ##__VA_ARGS__); \
        logger.log(buf);                                  \
    } while (0)
#else
// 3. 若未定义（发布模式）：定义 LOG_DEBUG 宏为空，不执行任何操作
#define LOG_DEBUG(logmsgFormat, ...)
#endif

class Logger : noncopyable {
public:
    // 获取日志唯一的实例对象
    static Logger &instance();

    // 设置日志等级
    void setLogLevel(int level);

    // 写日志
    void log(std::string msg);

private:
    int logLevel_; // 日志等级

};
~~~

**Logger.cc**
~~~
#include <iostream>
#include "Logger.h"
#include "Timestamp.h"

// 获取日志唯一的实例对象 单例
Logger &Logger::instance() {
    static Logger logger;
    return logger;
}

// 设置日志级别
void Logger::setLogLevel(int level) {
    logLevel_ = level;
}

// 写日志 [级别信息] time : msg
void Logger::log(std::string msg) {
    std::string pre = "";
    switch (logLevel_)
    {
    case INFO:
        pre = "[INFO]";
        break;
    case ERROR:
        pre = "[ERROR]";
        break;
    case FATAL:
        pre = "[FATAL]";
        break;
    case DEBUG:
        pre = "[DEBUG]";
        break;
    default:
        break;
    }

    // 打印时间和msg
    std::cout << pre + Timestamp::now().toString() << " : " << msg << std::endl;
}


// int main() {
//     LOG_INFO("hello world, %d", 12);
//     return 0;
// }
~~~

## 对时间戳的疯涨

Timestamp 类是对时间戳的封装，核心作用是方便地处理 
“自 epoch 时间（1970 年 1 月 1 日 00:00:00 UTC）到当前时间的微秒数”，
并提供时间获取、格式化等常用操作，是日志系统、性能统计、事件时序管理等场景的基础组件。

代码实现

**Timestamp.h**
~~~
#pragma once

#include <iostream>
#include <string>

class Timestamp
{
public:
    Timestamp();
    explicit Timestamp(int64_t microSecondsSinceEpoch);
    static Timestamp now();
    std::string toString() const; // const修饰，该函数只读

private:
    int64_t microSecondsSinceEpoch_; // 微妙级
};
~~~

**Timestamp.cc**
~~~
#include "Timestamp.h"

#include <time.h>

Timestamp::Timestamp() : microSecondsSinceEpoch_(0) {}

Timestamp::Timestamp(int64_t microSecondsSinceEpoch)
    : microSecondsSinceEpoch_(microSecondsSinceEpoch) {}

Timestamp Timestamp::now() {
    return Timestamp(time(NULL));
}
std::string Timestamp::toString() const {
    char buf[128] = {0};
    tm *tm_time = localtime(&microSecondsSinceEpoch_);
    snprintf(buf, 128, "%4d/%02d/%02d %02d:%02d:%02d",
             tm_time->tm_year + 1900,
             tm_time->tm_mon + 1,
             tm_time->tm_mday,
             tm_time->tm_hour,
             tm_time->tm_min,
             tm_time->tm_sec);
    return buf;
}

// int main() {
//     std::cout << Timestamp::now().toString() << std::endl;
//     return 0;
// }
~~~


## 线程相关实现

### CurrentThread

Linux系统中，线程的ID(tid) 需要通过 syscall(SYS_gettid) 获取， 但是系统级的调用耗时较高
若多线程的场景下频繁的获取tid， 会影响性能。
CurrentThread 命名空间通过一下设计解决问题

---
1. 保存tid缓存，线程本地存储（__thread） 每个线程一个
2. 首次获取时调用系统调用，后续直接返回缓存值
3. 编译器优化指令提升判断效率
---

代码实现
**CurrentThread.h**
~~~
#pragma once

#include <unistd.h>
#include <sys/syscall.h>

namespace CurrentThread {
    // __thread 用于声明线程本地存储变量
    // 声明变量为外部定义（实际定义在 .cpp 文件中），避免头文件重复定义
    extern __thread int t_cachedTid; // 保存tid缓存 因为系统调用非常耗时 拿到tid后将其保存

    void cacheTid();

    inline int tid() { // 内联函数只在当前文件中起作用 
        // __builtin_expect 是一种底层优化 此语句意思是如果还未获取tid 进入if 通过cacheTid()系统调用获取tid
        if (__builtin_expect(t_cachedTid == 0, 0)) {
            cacheTid();
        }
        return t_cachedTid;
    }
}
~~~
**CurrentThread.ccZ**
~~~
#include "CurrentThread.h"

namespace CurrentThread
{
    __thread int t_cachedTid = 0;

    void cacheTid() {
        // 首次获取，使用系统调用
        if (t_cachedTid == 0) {
            t_cachedTid = static_cast<pid_t>(::syscall(SYS_gettid));
        }
    }
}
~~~

### Thread 线程的封装

Thread 类是 C++ 中对线程操作的一个封装和增强。它的核心目标是让线程的创建、启动、管理和监控变得更加方便、安全和直观。
Thread 类，基于C++11 标准库的std::thread 进行封装，解决了std::thread 在实际项目中的几个痛点

---
1. 线程状态管理，提供started_ 和 joined_ 成员变量解决了这个问题
2. 线程标识，std::thread::id 是一个不透明的对象，
这个类，通过tid_ 成员变量存储了操作系统分配的线程id(tid)， 提供了tid() 方法
3. 线程命名
4. 禁止拷贝，线程是一个独立的执行单元
---

代码实现
**Thread.h**
~~~
#pragma once

#include <functional>
#include <thread>
#include <memory>
#include <string>
#include <atomic>

#include "noncopyable"

class Thread : noncopyable {
public:
    using ThreadFunc = std::function<void()>;
    explicit Thread(ThreadFunc, const std::string& name = std::string());
    ~Thread();

    void start();
    void join();

    bool started() {return started_; }
    pid_t tid() const {return tid_; }
    const std::string& name() const {return name_; }
    static int numCreated() {return numCreated_; }

private:
    void setDefaultName();

    // 线程状态
    bool started_;
    bool joined_;
    std::shared_ptr<std::thread> thread_; 
    pid_t tid_;       // 在线程创建时再绑定
    ThreadFunc func_; // 线程回调函数
    std::string name_; 
    static std::atomic_int numCreated_; // 计数
}
~~~
**Thread.cc**

~~~
#include "Thread.h"
#include "CurrentThread.h" 

#include <semaphore.h>

std::atomic_int Thread::numCreated_(0);

Thread::Thread(ThreadFunc func, const std::string name)
    : started_(false)
    , joined_(false)
    , tid_(0)
    , func_(std::move(func))
    , name_(name) 
{
    setDefaultName();
}
Thread::~Thread() {
    if (started_ && !joined_) {
        thread_->detach();
    }
}

void Thread::start()                                                        // 一个Thread对象 记录的就是一个新线程的详细信息
{
    started_ = true;
    sem_t sem;
    sem_init(&sem, false, 0);                                               // false指的是 不设置进程间共享
    // 开启线程
    thread_ = std::shared_ptr<std::thread>(new std::thread([&]() {
        tid_ = CurrentThread::tid();                                        // 获取线程的tid值
        sem_post(&sem);
        func_();                                                            // 开启一个新线程 专门执行该线程函数
    }));

    // 这里必须等待获取上面新创建的线程的tid值
    sem_wait(&sem);
}

// C++ std::thread 中join()和detach()的区别：https://blog.nowcoder.net/n/8fcd9bb6e2e94d9596cf0a45c8e5858a
void Thread::join()
{
    joined_ = true;
    thread_->join();
}

void Thread::setDefaultName() {
    int num = ++numCreated_;
    if (name_.empty()) {
        char buf[32] = {0};
        snprintf(buf, sizeof buf, "Thread%d", num);
        name_ = buf;
    }
}
~~~

阶段测试
编译指令：g++ test.cc ./src/CurrentThread.cc ./src/Thread.cc ./src/Timestamp.cc -o test -std=c++17 -I ./include
~~~
#include "Thread.h"
#include <iostream>
#include <chrono>

void my_task() {
    std::cout << "Thread " << CurrentThread::tid() << " is running." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "Thread " << CurrentThread::tid() << " finished." << std::endl;
}

int main() {
    Thread t1(my_task, "MyWorkerThread");
    
    std::cout << "Before start, Thread ID: " << t1.tid() << std::endl; // 可能输出 0，也可能因为未同步而不确定，但在本实现中，start()会等待，所以这里没问题？不，这里在start()之前调用，tid_还是0。
    
    t1.start();
    
    std::cout << "After start, Thread ID: " << t1.tid() << std::endl; // 一定能正确输出新线程的TID
    
    t1.join();
    
    std::cout << "Thread joined." << std::endl;
    
    return 0;
}
~~~

## InetAddress 和 Socket

### InetAddress
InetAddress 类是一个封装了 IPv4 socket 地址结构（sockaddr_in）的工具类
InetAddress 是网络编程中一个非常基础且实用的辅助类。
它将底层、繁琐的 C 语言 socket 地址操作封装成了优雅、安全的 C++ 方法，使得开发者能够更专注于业务逻辑，而不是底层的字节序转换和结构体操作。

~~~
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string>

// 封装socket地址类型
class InetAddress
{
public:
    // 构造函数 1：使用端口号和 IP 字符串创建对象
    explicit InetAddress(uint16_t port = 0, std::string ip = "127.0.0.1");
    // 构造函数 2：使用已有的 sockaddr_in 结构体创建对象
    explicit InetAddress(const sockaddr_in &addr)
        : addr_(addr)
    {
    }
    
    std::string toIp() const;
    std::string toIpPort() const;
    uint16_t toPort() const;

    const sockaddr_in *getSockAddr() const { return &addr_; }
    void setSockAddr(const sockaddr_in &addr) { addr_ = addr; }

private:
    sockaddr_in addr_; 
};
~~~

### Socket
Socket 类是对操作系统底层 socket 文件描述符（fd）的封装，遵循 noncopyable 语义（不可拷贝），核心作用是隐藏 socket 系统调用的底层细节，提供安全、简洁的 C++ 接口，用于创建、绑定、监听、接受连接等 TCP 通信核心操作。

~~~
#pragma once

#include "noncopyable.h"

class InetAddress;

// 封装socket fd
class Socket : noncopyable
{
public:
    explicit Socket(int sockfd)
        : sockfd_(sockfd)
    {
    }
    ~Socket();

    int fd() const { return sockfd_; }
    void bindAddress(const InetAddress &localaddr);
    void listen();
    int accept(InetAddress *peeraddr);

    void shutdownWrite();

    void setTcpNoDelay(bool on);
    void setReuseAddr(bool on);
    void setReusePort(bool on);
    void setKeepAlive(bool on);

private:
    const int sockfd_;
};

~~~

# 项目重点类实现细节

## Channel
这个 Channel 类是 Reactor 事件驱动模型中的核心组件之一，
是 **“事件分发器”或“I/O 对象代理”的角色。它的核心职责是封装一个文件描述符（fd）及其感兴趣的I/O 事件（如读、写），并在事件就绪时，将事件分发给事先注册好的回调函数**

代码实现细节与解释
**Channel.h**

~~~
#pragma once

#include <functional>
#include <memory>

#include "noncopyable.h"
#include "Timestamp.h"

class EventLoop; 

/**
 * 理清楚 EventLoop、Channel、Poller之间的关系  Reactor模型上对应多路事件分发器
 * Channel理解为通道 封装了sockfd和其感兴趣的event 如EPOLLIN、EPOLLOUT事件 还绑定了poller返回的具体事件
 **/
class Channel : noncopyable  // 继承noncopyable，禁止拷贝Channel对象
{
public:
    // 定义回调函数类型：
    // EventCallback 是无参无返回的函数对象，用于处理写、关闭、错误事件
    using EventCallback = std::function<void()>; 
    // ReadEventCallback 是带Timestamp参数的函数对象，用于处理读事件（时间戳记录事件发生时间）
    using ReadEventCallback = std::function<void(Timestamp)>;

    // 构造函数：初始化所属的EventLoop、文件描述符fd
    Channel(EventLoop *loop, int fd);
    ~Channel();  // 析构函数

    // 处理Poller通知的事件，在EventLoop::loop()中被调用
    void handleEvent(Timestamp receiveTime);

    // 设置各种事件的回调函数，使用std::move转移所有权，避免拷贝开销
    void setReadCallback(ReadEventCallback cb) { readCallback_ = std::move(cb); }
    void setWriteCallback(EventCallback cb) { writeCallback_ = std::move(cb); }
    void setCloseCallback(EventCallback cb) { closeCallback_ = std::move(cb); }
    void setErrorCallback(EventCallback cb) { errorCallback_ = std::move(cb); }

    // 绑定一个对象的shared_ptr，防止回调执行时对象被销毁（通过weak_ptr实现安全访问）
    void tie(const std::shared_ptr<void> &);

    // 以下是获取成员变量的内联函数：
    int fd() const { return fd_; }         // 获取封装的文件描述符
    int events() const { return events_; } // 获取感兴趣的事件集合
    void set_revents(int revt) { revents_ = revt; } // 设置实际发生的事件（由Poller调用）

    // 以下方法用于修改感兴趣的事件，并通知EventLoop更新Poller的监控
    void enableReading() { events_ |= kReadEvent; update(); }  // 启用读事件
    void disableReading() { events_ &= ~kReadEvent; update(); } // 禁用读事件
    void enableWriting() { events_ |= kWriteEvent; update(); }  // 启用写事件
    void disableWriting() { events_ &= ~kWriteEvent; update(); } // 禁用写事件
    void disableAll() { events_ = kNoneEvent; update(); }       // 禁用所有事件

    // 判断当前感兴趣的事件状态
    bool isNoneEvent() const { return events_ == kNoneEvent; }
    bool isWriting() const { return events_ & kWriteEvent; }
    bool isReading() const { return events_ & kReadEvent; }

    int index() { return index_; }         // 获取在Poller中的索引（用于Poller内部管理）
    void set_index(int idx) { index_ = idx; } // 设置在Poller中的索引

    EventLoop *ownerLoop() { return loop_; } // 获取所属的EventLoop
    void remove();  // 从EventLoop和Poller中移除当前Channel
private:

    void update();  // 通知EventLoop更新Poller对当前Channel的监控
    void handleEventWithGuard(Timestamp receiveTime); // 带对象生命周期保护的事件处理

    // 静态常量，定义事件类型（位掩码）
    static const int kNoneEvent;   // 无事件
    static const int kReadEvent;   // 读事件（EPOLLIN | EPOLLPRI）
    static const int kWriteEvent;  // 写事件（EPOLLOUT）

    EventLoop *loop_;  // 所属的EventLoop，Channel的所有操作都在该loop的线程中执行
    const int fd_;     // 封装的文件描述符，const表示fd一旦初始化不可修改
    int events_;       // 感兴趣的事件（由用户设置，如读、写）
    int revents_;      // 实际发生的事件（由Poller填充）
    int index_;        // 在Poller中的索引，用于Poller高效管理事件

    std::weak_ptr<void> tie_;  // 临时保护生命周期
    bool tied_;                // 标记是否已绑定对象

    // 事件发生时的回调函数
    ReadEventCallback readCallback_;   // 读事件回调
    EventCallback writeCallback_;      // 写事件回调
    EventCallback closeCallback_;      // 关闭事件回调
    EventCallback errorCallback_;      // 错误事件回调
};
~~~

**Channel.cc**
~~~
#include <sys/epoll.h>  // 包含epoll相关系统调用宏（如EPOLLIN、EPOLLOUT、EPOLLHUP等）

#include "Channel.h"    // 包含Channel类的声明
#include "EventLoop.h"  // 包含EventLoop类的声明（用于调用updateChannel/removeChannel）
#include "Logger.h"     // 日志工具类（用于打印事件处理日志）

// 初始化Channel类的静态常量（事件类型定义）
const int Channel::kNoneEvent = 0;                          // 空事件（无感兴趣的事件）
const int Channel::kReadEvent = EPOLLIN | EPOLLPRI;         // 读事件：EPOLLIN（普通数据可读）+ EPOLLPRI（紧急数据可读）
const int Channel::kWriteEvent = EPOLLOUT;                  // 写事件：EPOLLOUT（数据可写）

// Channel构造函数：初始化成员变量
// 参数：loop_（所属的EventLoop）、fd_（封装的文件描述符）
Channel::Channel(EventLoop *loop, int fd)
    : loop_(loop)    // 绑定当前Channel所属的EventLoop（一个Channel只能属于一个EventLoop）
    , fd_(fd)        // 赋值封装的fd（const修饰，一旦初始化不可修改）
    , events_(0)     // 初始化为空事件（默认不关注任何事件）
    , revents_(0)    // 初始化为空（实际发生的事件由Poller填充）
    , index_(-1)     // 初始化为-1（表示该Channel尚未注册到Poller中，用于Poller内部管理）
    , tied_(false)   // 初始化为false（表示未绑定任何对象的生命周期）
{
}

Channel::~Channel()
{
    // 析构函数为空：Channel不直接管理fd的生命周期（fd由TcpConnection等所有者管理）
    // 仅当Channel被remove后，才会从Poller中移除fd的监控
}

// 绑定一个对象的shared_ptr，通过weak_ptr保护对象生命周期
// 参数：obj（要绑定的对象的shared_ptr，通常是TcpConnection实例）
void Channel::tie(const std::shared_ptr<void> &obj)
{
    tie_ = obj;      // 将shared_ptr赋值给weak_ptr（weak_ptr不增加引用计数，不影响对象销毁）
    tied_ = true;    // 标记已绑定对象
}

// 通知EventLoop更新当前Channel在Poller中的监控状态（add/mod操作）
void Channel::update()
{
    // 调用所属EventLoop的updateChannel方法，间接委托Poller处理fd的事件注册/修改
    loop_->updateChannel(this);
}

// 通知EventLoop从Poller中移除当前Channel（del操作）
void Channel::remove()
{
    // 调用所属EventLoop的removeChannel方法，间接委托Poller删除fd的监控
    loop_->removeChannel(this);
}

// 核心方法：处理Poller通知的事件（在EventLoop::loop()中被调用）
// 参数：receiveTime（事件发生的时间戳，由Poller传递）
void Channel::handleEvent(Timestamp receiveTime)
{
    if (tied_)  // 如果已绑定对象（如TcpConnection），需要先检查对象是否存活
    {
        // 尝试将weak_ptr提升为shared_ptr：成功则对象存活，失败则对象已销毁
        std::shared_ptr<void> guard = tie_.lock();
        if (guard)  // 对象存活，执行带保护的事件处理
        {
            handleEventWithGuard(receiveTime);
        }
        // 若提升失败（对象已销毁），则不执行任何回调（避免访问已销毁对象）
    }
    else  // 未绑定对象，直接处理事件
    {
        handleEventWithGuard(receiveTime);
    }
}

// 带对象生命周期保护的事件处理逻辑（实际执行回调的核心）
void Channel::handleEventWithGuard(Timestamp receiveTime)
{
    // 打印日志：输出当前触发的事件类型（revents_是Poller填充的实际发生的事件）
    LOG_INFO("channel handleEvent revents:%d\n", revents_);

    // 1. 处理关闭事件（EPOLLHUP）：连接被对端关闭或挂起
    // 条件：触发EPOLLHUP且未触发EPOLLIN（避免重复处理）
    // 场景：如客户端调用close()关闭连接，服务端socket会触发EPOLLHUP
    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN))
    {
        if (closeCallback_)  // 若注册了关闭回调，执行回调
        {
            closeCallback_();
        }
    }

    // 2. 处理错误事件（EPOLLERR）：fd发生错误（如连接重置、fd无效）
    if (revents_ & EPOLLERR)
    {
        if (errorCallback_)  // 若注册了错误回调，执行回调
        {
            errorCallback_();
        }
    }

    // 3. 处理读事件（EPOLLIN：普通数据可读 / EPOLLPRI：紧急数据可读）
    if (revents_ & (EPOLLIN | EPOLLPRI))
    {
        if (readCallback_)  // 若注册了读回调，传入事件发生时间戳并执行
        {
            readCallback_(receiveTime);
        }
    }

    // 4. 处理写事件（EPOLLOUT：fd可写，如发送缓冲区有空闲空间）
    if (revents_ & EPOLLOUT)
    {
        if (writeCallback_)  // 若注册了写回调，执行回调
        {
            writeCallback_();
        }
    }
}
~~~


---
这份实现完整体现了 Channel 类的核心职责：
封装 fd 和事件（感兴趣事件 events_、实际发生事件 revents_）；
提供回调注册接口，在事件发生时按优先级分发回调；
通过 tie 机制保护对象生命周期，避免野指针访问；
委托 EventLoop 与 Poller 交互，解耦底层 I/O 多路复用实现。
---



## Poller

Poller 是 C++ 高性能网络编程（Reactor 模型）中的I/O 多路复用核心组件，
封装了 epoll/poll/select 等底层系统调用，核心职责是批量监听多个文件描述符（fd）的感兴趣事件，
当事件就绪时通知 EventLoop，是实现 “高并发” 的关键（避免为每个 fd 创建线程）。


该功能核心目标
~~~
高效管理大量 fd（支持成千上万个并发连接）。
阻塞等待就绪事件（减少 CPU 空轮询）。
将就绪事件快速通知 EventLoop，由 EventLoop 分发给对应 Channel 执行回调。
~~~

Poller 通常是抽象基类，具体实现由 EpollPoller（封装 epoll）、PollPoller（封装 poll）等派生类完成。
本项目只实现EpollPoller功能

**Poller.h**
~~~
#pragma once

#include <vector>
#include <unordered_map>

#include "noncopyable.h"
#include "Timestamp.h"

class Channel;
class EventLoop;

// muduo库中多路事件分发器的核心IO复用模块
class Poller
{
public:
    using ChannelList = std::vector<Channel *>;

    Poller(EventLoop *loop);
    virtual ~Poller() = default;

    // 给所有IO复用保留统一的接口
    virtual Timestamp poll(int timeoutMs, ChannelList *activeChannels) = 0;
    virtual void updateChannel(Channel *channel) = 0;
    virtual void removeChannel(Channel *channel) = 0;

    // 判断参数channel是否在当前的Poller当中
    bool hasChannel(Channel *channel) const;

    // EventLoop可以通过该接口获取默认的IO复用的具体实现
    static Poller *newDefaultPoller(EventLoop *loop);

protected:
    // map的key:sockfd value:sockfd所属的channel通道类型
    using ChannelMap = std::unordered_map<int, Channel *>;
    ChannelMap channels_;

private:
    EventLoop *ownerLoop_; // 定义Poller所属的事件循环EventLoop
};
~~~

**Poller.cc**

~~~
#include "Poller.h"
#include "Channel.h"

Poller::Poller(EventLoop *loop)
    : ownerLoop_(loop)
{
}

bool Poller::hasChannel(Channel *channel) const
{
    auto it = channels_.find(channel->fd());
    return it != channels_.end() && it->second == channel;
}
~~~

**补充DefaultPoller.cc**

~~~
这段代码是一个非常优雅的设计，它通过工厂模式封装了 Poller 对象的创建过程。其核心特点和意图如下：
封装实现细节：EventLoop 无需关心 Poller 的具体实现。
默认高性能：优先选择 epoll 作为 I/O 多路复用的后端，以获得最佳性能。
灵活性与可配置性：通过环境变量 MUDUO_USE_POLL，允许用户在必要时切换到 poll 实现，增强了代码的灵活性和可测试性。
可扩展性：增加新的 Poller 实现非常方便，符合 “开放 - 封闭原则”。
~~~

~~~
#include <stdlib.h>

#include "Poller.h"
#include "EPollPoller.h"

//由于只实现了 EPollPoller，所以这样实现
Poller *Poller::newDefaultPoller(EventLoop *loop)
{
    if (::getenv("MUDUO_USE_POLL"))
    {
        return nullptr; // 生成poll的实例
    }
    else
    {
        return new EPollPoller(loop); // 生成epoll的实例
    }
}
~~~

## EPollPoller 实现
EPollPoller 是 muduo 等 Reactor 模型框架中的核心组件，封装了 Linux 下的 epoll I/O 多路复用机制，
用于高效监听大量文件描述符（fd）的读写事件，是实现高并发网络编程的关键

Timestamp poll(int timeoutMs, ChannelList* activeChannels)
这个函数是Poller的核心，将事件监听器听到该fd发生的事件写进Channel中的revents成员变量中，
把这个Channel 装进activeChannels中，这样调用完之后能拿到事件监听器的监听结果

**EPollPoller.h**
~~~
#pragma once

#include <vector>
#include <sys/epoll.h>

#include "Poller.h"
#include "Timestamp.h"

class Channel;

// EpollPoller 是 Poller 的子类，封装了 epoll 的功能实现
class EPollPoller : public Poller {
public:
    // 构造函数：传入所属的 EventLoop
    EPollPoller(EventLoop* loop);
    // 析构函数：override 确保正确重写基类析构
    ~EPollPoller() override;

    // 重写基类的抽象方法，实现 epoll_wait 逻辑，返回活跃事件的时间戳
    Timestamp poll(int timeoutMs, ChannelList* activeChannels) override;
    // 重写基类方法，更新 Channel 在 epoll 中的事件监控状态
    void updateChannel(Channel *channel) override;
    // 重写基类方法，将 Channel 从 epoll 监控中移除
    void removeChannel(Channel *channel) override;

private:
    // 定义 epoll_event 向量的初始容量，避免频繁扩容
    static const int kInitEventListSize = 16;

    // 核心辅助方法：将 epoll_wait 返回的就绪事件填充到 activeChannels（传出参数）
    void fillActiveChannels(int numEvents, ChannelList *activeChannels) const;
    // 底层辅助方法：调用 epoll_ctl 执行 ADD/MOD/DEL 操作，更新 epoll 内部状态
    void update(int operation, Channel *channel);

    // 类型别名：简化 epoll_event 向量的写法，提高代码可读性
    using EventList = std::vector<epoll_event>;

    int epollfd_; // epoll 实例的文件描述符（由 epoll_create 创建）
    EventList events_; // 存储 epoll_wait 返回的就绪事件列表
};
~~~

具体功能实现
**EPollPoller.cc**
~~~
#include "EPollPoller.h"
#include "Logger.h"
#include "Channel.h"

#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

const int kNew = -1;    // 某个channel还没添加至Poller          // channel的成员index_初始化为-1
const int kAdded = 1;   // 某个channel已经添加至Poller
const int kDeleted = 2; // 某个channel已经从Poller删除

EPollPoller::EPollPoller(EventLoop *loop) 
    :Poller(loop)
    ,epollfd_(::epoll_create(EPOLL_CLOEXEC))
    ,events_(kInitEventListSize)  // std::vector<epoll_event>(16)
{
    if (epollfd_ < 0) {
        LOG_FATAL("epoll_create error:%d\n", errno);
    }
}
EPollPoller::~EPollPoller() {
    ::close(epollfd_);
}

// 监听
Timestamp EPollPoller::poll(int timeoutMs, ChannelList *activeChannels) {
    // 由于频繁调用poll 实际上应该用LOG_DEBUG输出日志更为合理 当遇到并发场景 关闭DEBUG日志提升效率
    LOG_INFO("func=%s => fd total count:%lu\n", __FUNCTION__, channels_.size());

    int numEvents = ::epoll_wait(epollfd_, &*events_.begin(), static_cast<int>(events_.size()), timeoutMs);
    int saveErrno = errno;
    // 返回事件发生的精准时间
    Timestamp now(Timestamp::now());

    if (numEvents > 0) {
        LOG_INFO("%d events happend\n", numEvents); // LOG_DEBUG最合理
        fillActiveChannels(numEvents, activeChannels);
        if (numEvents == events_.size()) {
            // 扩容操作
            events_.resize(events_.size() * 2);
        }
    } else if (numEvents == 0) {
        LOG_DEBUG("%s timeout!\n", __FUNCTION__);
    } else {
        if (saveErrno != EINTR) {
            errno = saveErrno;
            LOG_ERROR("EPollPoller::poll() error!");
        }
    }
    return now;
}
// 重写基类方法，更新 Channel 在 epoll 中的事件监控状态
void EPollPoller::updateChannel(Channel *channel) {
    const int index = channel->index();
    LOG_INFO("func=%s => fd=%d events=%d index=%d\n", __FUNCTION__, channel->fd(), channel->events(), index);

    if (index == kNew || index == kDeleted) {
        if (index == kNew) {
            int fd = channel->fd();
            channels_[fd] = channel;
        } else { // index == kDeleted
        } 
        channel->set_index(kAdded); // 将状态设置为“已添加”
        update(EPOLL_CTL_ADD, channel); // 调用底层update函数，执行epoll_ctl ADD操作
    } else {
        // channel已经在Poller中注册过了
        // 断言：确保在channels_映射表中能找到这个fd，这是一个一致性检查
        assert(channels_.find(fd) != channels_.end());
        assert(channels_[fd] == channel);
        int fd = channel->fd();
        if (channel->isNoneEvent()) {
            update(EPOLL_CTL_DEL, channel);
            channel->set_index(kDeleted);
        }
        else {
            update(EPOLL_CTL_MOD, channel);
        }
    }
}
// 重写基类方法，将 Channel 从 epoll 监控中移除
void EPollPoller::removeChannel(Channel *channel) {
    int fd = channel->fd();
    channels_.erase(fd);

    LOG_INFO("func=%s => fd=%d\n", __FUNCTION__, fd);

    int index = channel->index();
    if (index == kAdded)
    {
        update(EPOLL_CTL_DEL, channel);
    }
    channel->set_index(kNew);
}


// 核心辅助方法：将 epoll_wait 返回的就绪事件填充到 activeChannels（传出参数）
// 填写活跃的连接
void EPollPoller::fillActiveChannels(int numEvents, ChannelList *activeChannels) const
{
    // 遍历所有epoll返回的就绪事件
    for (int i = 0; i < numEvents; ++i)
    {
        // 1. 从epoll_event中获取Channel指针
        // events_[i] 是一个 struct epoll_event
        // events_[i].data.ptr 是我们在调用epoll_ctl时，通过 event.data.ptr 字段存入的 Channel*
        // 这里是将其强制转换回 Channel* 类型
        Channel *channel = static_cast<Channel *>(events_[i].data.ptr);
        
        // 2. 将epoll返回的具体事件类型设置到Channel对象中
        // events_[i].events 是一个整数，代表了发生的事件类型（如 EPOLLIN, EPOLLOUT, EPOLLERR）
        // channel->set_revents(...) 将这个事件类型存入Channel的 revents_ 成员变量
        // 这个 revents_ 就是Channel的 handleEvent() 方法判断应该执行哪个回调的依据
        channel->set_revents(events_[i].events);
        
        // 3. 将就绪的Channel添加到输出列表中
        // activeChannels 是一个由 EventLoop 传入的空列表
        // push_back(channel) 将当前这个发生了事件的Channel指针添加到列表末尾
        activeChannels->push_back(channel); // EventLoop就拿到了它的Poller给它返回的所有发生事件的channel列表了
    }
}

// 底层辅助方法：调用 epoll_ctl 执行 ADD/MOD/DEL 操作，更新 epoll 内部状态
void EPollPoller::update(int operation, Channel *channel) {
    epoll_event event; 
    ::memset(&event, 0, sizeof(event));

    int fd = channel->fd();

    event.events = channel->events();
    event.data.fd = fd;
    //这是最关键的一步！将 Channel 对象的指针存入 event.data.ptr 字段。
    // 当这个 fd 上有事件发生时，epoll_wait 会返回这个 event 结构体，
    // 我们就可以通过 event.data.ptr 快速地找到对应的 Channel 对象，
    // 而无需再通过 fd 去 channels_ 映射表中查找。这是 Channel 和 epoll_event 之间的直接桥梁。
    event.data.ptr = channel;

    if (::epoll_ctl(epollfd_, operation, fd, &event) < 0) {
        if (operation == EPOLL_CTL_DEL) {
            LOG_ERROR("epoll_ctl del error:%d\n", errno);
        } else {
            LOG_FATAL("epoll_ctl add/mod error:%d\n", errno);
        }
    }
}
~~~

## EventLoop
作为一个网络服务器，需要有持续监听，持续获取监听结果，持续处理监听结果对应事件的能力
也就是我们需要循环的去 **调用Poller::poll方法获取实际发生事件的Channel集合** 然后
调用这些Channel里面保管的不同类型事件的处理函数
EventLoop就是负责实现 “循环” ，负责驱动 “循环” 的重要模块
这个类整合封装了二者并向上提供了更方便的接口

**EventLoop.h**
~~~
#pragma once

#include <functional>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>
 
#include "noncopyable.h"
#include "Timestamp.h"
#include "CurrentThread.h"

// 前置声明，避免循环依赖
class Channel;
class Poller;

/**
 * @brief 事件循环类 (Reactor 模式的核心)
 * 
 * 一个 EventLoop 实例代表一个独立的事件循环线程。它负责：
 * 1. 通过 Poller (I/O 多路复用器) 等待 I/O 事件（如 socket 可读、可写）。
 * 2. 当事件发生时，调用相应 Channel 的回调函数进行处理。
 * 3. 执行用户通过 runInLoop/queueInLoop 投递的任务（Functor）。
 * 
 * 核心特性：
 * - 每个 EventLoop 对象绑定一个线程，通过 threadId_ 标识。
 * - 线程安全的任务队列 (pendingFunctors_)，允许跨线程投递任务。
 * - 使用 wakeupFd_ 实现高效的跨线程唤醒。
 * 
 * 注意：
 * - 一个线程只能拥有一个 EventLoop 对象。
 * - EventLoop 的生命周期通常由其所属的线程（如 TcpServer 的主循环线程）管理。
 */
class EventLoop : noncopyable
{
public:
    // 定义任务回调类型
    using Functor = std::function<void()>;

    /**
     * @brief 构造函数
     * 
     * 在构造时，会：
     * 1. 记录当前线程 ID (threadId_)。
     * 2. 创建 Poller 对象 (epoll/poll 的封装)。
     * 3. 创建 wakeupFd_ (通过 eventfd) 并封装成 wakeupChannel_。
     * 4. 将 wakeupChannel_ 的读事件回调设置为 handleRead，用于唤醒事件循环。
     */
    EventLoop();
    

    /**
     * @brief 析构函数
     * 
     * 确保在析构时事件循环已经停止 (quit_ 为 true)。
     */
    ~EventLoop();

    /**
     * @brief 启动事件循环的主循环
     * 
     * 这是 EventLoop 的核心方法，调用后会进入一个无限循环，直到 quit() 被调用。
     * 循环逻辑：
     * 1. 调用 Poller::poll() 阻塞等待事件，超时时间由 kPollTimeMs 指定。
     * 2. poll() 返回后，获取就绪的 Channel 列表 (activeChannels_)。
     * 3. 遍历 activeChannels_，调用每个 Channel 的 handleEvent() 方法处理事件。
     * 4. 执行 doPendingFunctors()，处理在此期间被投递到队列中的任务。
     * 
     * 注意：此方法会阻塞调用线程，通常在一个线程的主函数中调用。
     */
    void loop();

    /**
     * @brief 安全地退出事件循环
     * 
     * 设置 quit_ 标志为 true。如果调用此方法的线程不是 EventLoop 所属的线程，
     * 必须唤醒事件循环线程，使其从 poll() 的阻塞中返回，以便检查 quit_ 标志并退出。
     */
    void quit();

    /**
     * @brief 获取最近一次 poll() 返回的时间戳
     * @return Timestamp 最近一次 I/O 事件就绪的时间
     */
    Timestamp pollReturnTime() const;

    /**
     * @brief 在 EventLoop 所属的线程中执行一个任务
     * 
     * 如果调用此方法的线程就是 EventLoop 所属的线程，则立即执行任务 cb。
     * 否则，将任务 cb 投递到任务队列 pendingFunctors_ 中，并唤醒事件循环线程以尽快执行。
     * 
     * @param cb 要执行的任务回调
     */
    void runInLoop(Functor cb);

    /**
     * @brief 将一个任务投递到 EventLoop 的任务队列中，待后续执行
     * 
     * 此方法是线程安全的。它会将任务 cb 加入 pendingFunctors_，
     * 并根据情况决定是否需要唤醒事件循环线程。
     * 唤醒的条件是：
     * 1. 调用线程不是 EventLoop 所属线程。
     * 2. 事件循环正在执行任务队列 (callingPendingFunctors_ 为 true)，以避免新任务被延迟到下一轮 poll。
     * 
     * @param cb 要投递的任务回调
     */
    void queueInLoop(Functor cb);

    /**
     * @brief 唤醒 EventLoop 线程
     * 
     * 通过向 wakeupFd_ 写入一个字节的数据，使阻塞在 poll() 上的 EventLoop 线程立即返回。
     * 这是实现跨线程唤醒 EventLoop 的标准方法。
     */
    void wakeup();

    /**
     * @brief 更新 Channel 的事件注册
     * 
     * 此方法是 Channel 和 Poller 之间的桥梁。当 Channel 的感兴趣事件发生变化时，
     * 它会调用此方法通知 Poller 更新其内部的数据结构 (如 epoll_ctl)。
     * 
     * @param channel 需要更新的 Channel 对象
     */
    void updateChannel(Channel *channel);

    /**
     * @brief 将 Channel 从 Poller 的监听列表中移除
     * 
     * 当一个连接关闭时，TcpConnection 会调用此方法将其对应的 Channel 从 EventLoop 中注销。
     * 
     * @param channel 需要移除的 Channel 对象
     */
    void removeChannel(Channel *channel);

    /**
     * @brief 检查 EventLoop 是否正在监听某个特定的 Channel
     * @param channel 要检查的 Channel 对象
     * @return true 如果正在监听，false 否则
     */
    bool hasChannel(Channel *channel);

    /**
     * @brief 检查当前调用线程是否是 EventLoop 所属的线程
     * @return true 如果是，false 否则
     */
    bool isInLoopThread() const;

private:
    /**
     * @brief wakeupFd_ 的读事件回调函数
     * 
     * 当其他线程调用 wakeup() 向 wakeupFd_ 写入数据时，此回调会被触发。
     * 它的主要工作是从 wakeupFd_ 中读取数据（通常是 8 字节），以清空内核缓冲区，
     * 使 wakeupFd_ 可以再次用于唤醒。它本身不执行任何业务逻辑。
     */
    void handleRead();

    /**
     * @brief 执行 pendingFunctors_ 队列中的所有任务
     * 
     * 为了避免在执行任务的过程中，任务队列被反复加锁（特别是当任务本身又调用 queueInLoop 时），
     * 此函数采用了“ swap and dispatch ”的策略：
     * 1. 用一个局部向量 functors 与 pendingFunctors_ 进行交换。
     * 2. 解锁，这样其他线程可以继续向 pendingFunctors_ 中添加任务。
     * 3. 遍历局部向量 functors 并执行其中的所有任务。
     * 
     * callingPendingFunctors_ 标志在任务执行期间被设为 true，用于 queueInLoop 判断是否需要唤醒。
     */
    void doPendingFunctors();

    // --- 成员变量 ---

    /// @brief 标记事件循环是否正在运行
    std::atomic_bool looping_;

    /// @brief 标记是否需要退出事件循环
    std::atomic_bool quit_;

    /// @brief 记录当前 EventLoop 所属的线程 ID
    const pid_t threadId_;

    /// @brief 记录最近一次 poll() 返回的时间戳
    Timestamp pollReturnTime_;

    /// @brief 指向 Poller 对象的智能指针，由 EventLoop 唯一拥有
    std::unique_ptr<Poller> poller_;

    /// @brief 用于跨线程唤醒的文件描述符，由 eventfd() 创建
    int wakeupFd_;

    /// @brief 封装 wakeupFd_ 的 Channel 对象
    std::unique_ptr<Channel> wakeupChannel_;

    /// @brief 存储每次 poll() 返回的就绪 Channel 列表
    ChannelList activeChannels_;

    /// @brief 保护 pendingFunctors_ 的互斥锁
    std::mutex mutex_;

    /// @brief 存储等待被执行的任务队列
    std::vector<Functor> pendingFunctors_;

    /// @brief 标记当前是否正在执行 pendingFunctors_ 中的任务
    /// 用于在 queueInLoop 中判断，当任务在执行期间又有新任务到来时，需要唤醒以避免延迟。
    std::atomic_bool callingPendingFunctors_;
};
~~~

以上一共实现了三个模块的实现，Poller，Channel，EventLoop。
1. EventLoop
角色：驱动循环的核心。它通过不断调用 Poller 监听事件，获取就绪事件后触发 Channel 的回调，
同时处理异步任务队列，是整个事件驱动流程的 “发动机”。
2. Poller
角色：事件监听器的结果获取者。它封装了 epoll 或 poll 等 I/O 多路复用机制，
负责从操作系统层面获取哪些文件描述符（fd）发生了感兴趣的事件（如可读、可写），并将结果反馈给 EventLoop。
3. Channel
角色：fd 及其属性的封装者。
它将 fd、感兴趣的事件（如 EPOLLIN）、实际发生的事件（如 revents_）以及对应的回调函数（读、写、关闭、错误回调）整合在一起，
使得 fd 的事件管理和回调触发逻辑更加内聚，在 EventLoop 和 Poller 之间起到了 “桥梁” 作用，方便模块间的事件传递与处理。


**One Loop Per Thresd含义引入**
每一个EventLoop都绑定了一个线程（一对一绑定），这种运行模式是Muduo库的特色充分利用了多核cpu的能力。
muduo 通过 “一线程一循环” 的设计，既利用了多核的性能，又简化了线程安全的复杂度 —— 这是它能成为高性能网络库的关键设计之一。




## Acceptor

Acceptor封装了服务器监听套接字fd以及相关处理方法，
这个类主要是对其他方法调用的封装

Acceptor 是 Reactor 模型中的连接接收器核心组件，专门负责监听指定端口、接收客户端新连接，并将连接分发交给上层处理（如 TcpServer），是 TCP 服务器接收连接的 “入口”。

作用： 监听端口 + 接受连接，屏蔽底层Socket，Channel的写作细节，
向上提供简洁的新连接回调接口

依赖 Socket：管理监听用的 socket（acceptSocket_），负责绑定、监听端口。
依赖 Channel：监听 acceptSocket_ 的读事件（有新连接时触发），并回调 handleRead() 处理。
依赖 EventLoop：运行在指定的事件循环中（通常是 mainLoop），由 EventLoop 驱动事件响应。

**Acceptor.h**

~~~
#pragma once

#include <functional>

#include "noncopyable.h"
#include "Socket.h"
#include "Channel.h"

class EventLoop;
class InetAddress;

class Acceptor : noncopyable
{
public:
    using NewConnectionCallback = std::function<void(int sockfd, const InetAddress &)>;

    Acceptor(EventLoop *loop, const InetAddress &listenAddr, bool reuseport);
    ~Acceptor();
    //设置新连接的回调函数
    void setNewConnectionCallback(const NewConnectionCallback &cb) { NewConnectionCallback_ = cb; }
    // 判断是否在监听
    bool listenning() const { return listenning_; }
    // 监听本地端口
    void listen();

private:
    void handleRead();//处理新用户的连接事件

    EventLoop *loop_; // Acceptor用的就是用户定 义的那个baseLoop 也称作mainLoop
    Socket acceptSocket_;//专门用于接收新连接的socket
    Channel acceptChannel_;//专门用于监听新连接的channel
    NewConnectionCallback NewConnectionCallback_;//新连接的回调函数
    bool listenning_;//是否在监听
};
~~~

**Acceptor.cc**
~~~
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <unistd.h>

#include "Acceptor.h"
#include "Logger.h"
#include "InetAddress.h"

static int createNonblocking()
{
    /*
        SOCK_STREAM：TCP 流式协议（面向连接、可靠传输）。
        SOCK_NONBLOCK：设置 fd 为非阻塞模式，避免 accept 等操作阻塞 EventLoop。
        SOCK_CLOEXEC：进程执行 exec 系统调用时自动关闭该 fd，避免 fd 泄漏到子进程。
    */
    int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (sockfd < 0)
    {
        LOG_FATAL("%s:%s:%d listen socket create err:%d\n", __FILE__, __FUNCTION__, __LINE__, errno);
    }
    return sockfd;
}

Acceptor::Acceptor(EventLoop *loop, const InetAddress &listenAddr, bool reuseport)
    : loop_(loop)
    , acceptSocket_(createNonblocking())  // 初始化监听socket
    , acceptChannel_(loop, acceptSocket_.fd()) // 绑定loop和监听 fd
    , listenning_(false) 
{
    acceptSocket_.setReuseAddr(true);   
    acceptSocket_.setReusePort(true);
    acceptSocket_.bindAddress(listenAddr); // 绑定监听地址
    // TcpServer::start() => Acceptor.listen() 如果有新用户连接 要执行一个回调(accept => connfd => 打包成Channel => 唤醒subloop)
    // baseloop监听到有事件发生 => acceptChannel_(listenfd) => 执行该回调函数
    acceptChannel_.setReadCallback(
        std::bind(&Acceptor::handleRead, this));
}

Acceptor::~Acceptor()
{
    acceptChannel_.disableAll();    // 把从Poller中感兴趣的事件删除掉
    acceptChannel_.remove();        // 调用EventLoop->removeChannel => Poller->removeChannel 把Poller的ChannelMap对应的部分删除
}

void Acceptor::listen()
{
    listenning_ = true;
    acceptSocket_.listen();         // listen
    acceptChannel_.enableReading(); // acceptChannel_注册至Poller !重要
}

// listenfd有事件发生了，就是有新用户连接了
void Acceptor::handleRead()
{
    InetAddress peerAddr;  //存客户端地址
    int connfd = acceptSocket_.accept(&peerAddr);
    if (connfd >= 0)
    {
        if (NewConnectionCallback_) // 如果设置了回调
        {
            NewConnectionCallback_(connfd, peerAddr); // 轮询找到subLoop 唤醒并分发当前的新客户端的Channel
        }
        else
        {
            ::close(connfd);
        }
    }
    else
    {
        LOG_ERROR("%s:%s:%d accept err:%d\n", __FILE__, __FUNCTION__, __LINE__, errno);
        if (errno == EMFILE)
        {
            LOG_ERROR("%s:%s:%d sockfd reached limit\n", __FILE__, __FUNCTION__, __LINE__);
        }
    }
}

~~~

## 补充工具类 Buffer
Buffer 类是网络编程中一种高效的内存缓冲区管理工具，核心设计目标是减少内存拷贝、优化 I/O 操作性能，同时提供安全、便捷的读写接口。

**核心设计思想**

~~~
Buffer 的设计基于 “预分配 + 读写索引分离 + 动态扩容” 模式，解决了直接使用 std::vector 或 C 数组的三大痛点：
减少内存拷贝：通过读写索引分离，避免每次读写后的数据移动（如读取数据后无需 memcpy 前移剩余数据）；
优化 I/O 效率：预留 kCheapPrepend 空间，适配 TCP 协议头（如在数据前添加长度字段），同时支持从 fd 直接读写（readFd/writeFd），减少系统调用次数；
动态扩容策略：根据需要自动调整缓冲区大小，平衡内存占用和扩容开销
~~~

**Buffer.h**
~~~
#pragma once

#include <vector>
#include <string>
#include <algorithm>
#include <stddef.h>

// 网络库底层的缓冲区类型定义
class Buffer
{
public:
    static const size_t kCheapPrepend = 8;//初始预留的prependabel空间大小
    static const size_t kInitialSize = 1024; // 

    explicit Buffer(size_t initalSize = kInitialSize)
        : buffer_(kCheapPrepend + initalSize)
        , readerIndex_(kCheapPrepend)
        , writerIndex_(kCheapPrepend)
    {
    }

    // 可读数据长度
    size_t readableBytes() const { return writerIndex_ - readerIndex_; }
    // 可写空间长度
    size_t writableBytes() const { return buffer_.size() - writerIndex_; }
    //前置预留空间长度
    size_t prependableBytes() const { return readerIndex_; }

    // 返回缓冲区中可读数据的起始地址
    const char *peek() const { return begin() + readerIndex_; }

    // 读取指定长度数据
    void retrieve(size_t len)
    {
        if (len < readableBytes())
        {
            readerIndex_ += len; // 说明应用只读取了可读缓冲区数据的一部分，就是len长度 还剩下readerIndex+=len到writerIndex_的数据未读
        }
        else // len == readableBytes()
        {
            retrieveAll(); // 读完所有数据，复位缓冲区
        }
    }
    // 读完所有数据，复位缓冲区
    void retrieveAll()
    {
        readerIndex_ = kCheapPrepend;
        writerIndex_ = kCheapPrepend;
    }

    // 把onMessage函数上报的Buffer数据 转成string类型的数据返回
    std::string retrieveAllAsString() { return retrieveAsString(readableBytes()); }

    // 读取数据并转为字符串
    std::string retrieveAsString(size_t len)
    {
        std::string result(peek(), len);
        retrieve(len); // 上面一句把缓冲区中可读的数据已经读取出来 这里肯定要对缓冲区进行复位操作
        return result;
    }

    // buffer_.size - writerIndex_
    void ensureWritableBytes(size_t len)
    {
        if (writableBytes() < len)
        {
            makeSpace(len); // 扩容
        }
    }

    // 把[data, data+len]内存上的数据添加到writable缓冲区当中
    void append(const char *data, size_t len)
    {
        ensureWritableBytes(len);
        std::copy(data, data+len, beginWrite());
        writerIndex_ += len;
    }
    char *beginWrite() { return begin() + writerIndex_; }
    const char *beginWrite() const { return begin() + writerIndex_; }

    // 从fd上读取数据
    ssize_t readFd(int fd, int *saveErrno);
    // 通过fd发送数据
    ssize_t writeFd(int fd, int *saveErrno);

private:
    // vector底层数组首元素的地址 也就是数组的起始地址
    char *begin() { return &*buffer_.begin(); }
    const char *begin() const { return &*buffer_.begin(); }

    void makeSpace(size_t len)
    {
        /**
         * | kCheapPrepend |xxx| reader | writer |                     // xxx标示reader中已读的部分
         * | kCheapPrepend | reader ｜          len          |
         **/
        if (writableBytes() + prependableBytes() < len + kCheapPrepend) // 也就是说 len > xxx前面剩余的空间 + writer的部分
        {
            buffer_.resize(writerIndex_ + len);
        }
        else // 这里说明 len <= xxx + writer 把reader搬到从xxx开始 使得xxx后面是一段连续空间
        {
            size_t readable = readableBytes(); // readable = reader的长度
            // 将当前缓冲区中从readerIndex_到writerIndex_的数据
            // 拷贝到缓冲区起始位置kCheapPrepend处，以便腾出更多的可写空间
            std::copy(begin() + readerIndex_,
                      begin() + writerIndex_,
                      begin() + kCheapPrepend);
            readerIndex_ = kCheapPrepend;
            writerIndex_ = readerIndex_ + readable;
        }
    }

    std::vector<char> buffer_;
    size_t readerIndex_;
    size_t writerIndex_;
};
~~~

## EventLoopThread
在多线程网络编程中，为了充分利用多核 CPU，通常会采用主从 Reactor 模型或多 Reactor 模型。EventLoopThread 就是用来创建 “从 Reactor”（Sub Reactor）的工具。

EventLoopThread 是一个将EventLoop和线程绑定在一起的辅助类，核心作用是
创建一个新线程，并在这个线程启动一个EventLoop的事件循环，这是实现One Loop Per Thread
模型的关键组件

**EventLoopThread.h**

~~~
#pragma once

#include <functional>
#include <mutex>
#include <condition_variable>
#include <string>

#include "noncopyable.h"
#include "Thread.h"

class EventLoop;

class EventLoopThread : noncopyable
{
public:
    using ThreadInitCallback = std::function<void(EventLoop *)>;

    EventLoopThread(const ThreadInitCallback &cb = ThreadInitCallback(),
                    const std::string &name = std::string());
    ~EventLoopThread();
    
    EventLoop *startLoop(); // 主线程执行startLoop，创建新线程，开启loop

private:
    /*
    核心流程（新线程独立执行）：
    创建 EventLoop 对象：在新线程的栈上分配，生命周期与线程绑定，无需手动释放。
    执行初始化回调：若用户设置了 ThreadInitCallback，在 loop() 启动前执行（如注册初始 Channel、设置线程局部存储等）。
    同步主线程：加锁后给 loop_ 赋值，通过条件变量唤醒主线程，让主线程获取 EventLoop 指针。
    启动事件循环：调用 loop.loop()，进入无限循环（监听 Poller 事件、处理回调），直到 loop.quit() 被调用。
    循环退出后重置 loop_：避免主线程通过已失效的 loop_ 指针操作。
    */
    void threadFunc(); // 创建并执行EventLoop

    EventLoop *loop_; // 指向本线程中创建的 EventLoop 对象
    bool exiting_; // 标记线程是否正在退出
    Thread thread_;  // 封装了线程的创建和线程的启动
    /*
    用 std::mutex + std::condition_variable 解决 “主线程等待新线程初始化 EventLoop” 的同步问题。
    cond_.wait() 带条件判断（loop_ != nullptr），避免虚假唤醒（如系统信号导致的误唤醒）。
    */
    std::mutex mutex_;             // 互斥锁
    std::condition_variable cond_; // 条件变量
    ThreadInitCallback callback_;  // 线程初始化回调函数
};
~~~

细节实现
**EventLoopThread.cc**
~~~
#include "EventLoopThread.h"
#include "EventLoop.h"


/*
初始化成员变量：loop_（指向线程内的 EventLoop）设为 nullptr，exiting_（退出标志）设为 false。
初始化线程对象 thread_：通过 std::bind 将 threadFunc（线程入口函数）与当前对象绑定，同时指定线程名称（方便调试日志）。
保存线程初始化回调 callback_：用于 EventLoop 启动前的自定义初始化。
*/
EventLoopThread::EventLoopThread(const ThreadInitCallback &cb,
                                 const std::string &name)
    : loop_(nullptr)
    , exiting_(false)
    , thread_(std::bind(&EventLoopThread::threadFunc, this), name)
    , mutex_()
    , cond_()
    , callback_(cb)
{ 
}

EventLoopThread::~EventLoopThread()
{
    exiting_ = true;
    if (loop_ != nullptr)
    {
        loop_->quit(); // 通知EventLoop退出事件循环
        thread_.join(); // 等待线程执行完毕，避免资源泄漏
    }
}

EventLoop *EventLoopThread::startLoop()
{
    thread_.start(); // 启用底层线程Thread类对象thread_中通过start()创建的线程

    EventLoop *loop = nullptr;
    {
        // 加锁等待
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this](){return loop_ != nullptr;});
        loop = loop_;
    }
    return loop;
}

// 下面这个方法 是在单独的新线程里运行的
void EventLoopThread::threadFunc()
{
    EventLoop loop; // 创建一个独立的EventLoop对象 和上面的线程是一一对应的 级one loop per thread

    // 初始化回调函数，如果有
    if (callback_)
    {
        callback_(&loop);
    }

    {
        // 加锁
        std::unique_lock<std::mutex> lock(mutex_);
        loop_ = &loop;
        // 通知
        cond_.notify_one();
    }
    loop.loop();    // 执行EventLoop的loop() 开启了底层的Poller的poll()
    std::unique_lock<std::mutex> lock(mutex_);
    loop_ = nullptr;
}
~~~
