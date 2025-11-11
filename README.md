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