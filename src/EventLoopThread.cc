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