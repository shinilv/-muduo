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