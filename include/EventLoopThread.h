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