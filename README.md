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



# 项目细节

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

