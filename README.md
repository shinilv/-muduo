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
---
高效管理大量 fd（支持成千上万个并发连接）。
阻塞等待就绪事件（减少 CPU 空轮询）。
将就绪事件快速通知 EventLoop，由 EventLoop 分发给对应 Channel 执行回调。
---

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