#include <memory>
 
#include "EventLoopThreadPool.h"
#include "EventLoopThread.h"
#include "Logger.h"

/*
保存主循环 baseLoop_（主线程的 EventLoop，即 Main Reactor）。
保存线程池名称 name_（用于调试日志，区分不同线程池）。
初始化状态变量：started_（是否启动）设为 false，numThreads_（线程数）初始为 0，next_（轮询索引）初始为 0。
*/
EventLoopThreadPool::EventLoopThreadPool(EventLoop *baseLoop, const std::string &nameArg)
    : baseLoop_(baseLoop), name_(nameArg), started_(false), numThreads_(0), next_(0)
{
}

EventLoopThreadPool::~EventLoopThreadPool()
{
    // Don't delete loop, it's stack variable
}

void EventLoopThreadPool::start(const ThreadInitCallback &cb)
{
    started_ = true;

    for (int i = 0; i < numThreads_; ++i)
    {
        char buf[name_.size() + 32];
        snprintf(buf, sizeof buf, "%s%d", name_.c_str(), i); // 生成线程名
        EventLoopThread *t = new EventLoopThread(cb, buf); // 创建EventLoopThread
        threads_.push_back(std::unique_ptr<EventLoopThread>(t)); // 智能指针管理，避免泄漏
        loops_.push_back(t->startLoop()); // 底层创建线程 绑定一个新的EventLoop 并返回该loop的地址
    }
    // 单线程模式：线程数为 0，直接在 baseLoop_ 执行初始化回调
    if (numThreads_ == 0 && cb) // 整个服务端只有一个线程运行baseLoop
    {
        cb(baseLoop_);
    }
}

// 如果工作在多线程中，baseLoop_(mainLoop)会默认以轮询的方式分配Channel给subLoop
EventLoop *EventLoopThreadPool::getNextLoop()
{
    // 如果只设置一个线程 也就是只有一个mainReactor 无subReactor 
    // 那么轮询只有一个线程 getNextLoop()每次都返回当前的baseLoop_
    EventLoop *loop = baseLoop_;    

    // 通过轮询获取下一个处理事件的loop
    // 如果没设置多线程数量，则不会进去，相当于直接返回baseLoop
    if(!loops_.empty())             
    {
        loop = loops_[next_];
        ++next_;
        // 轮询
        if(next_ >= loops_.size())  // 索引越界后重置，实现轮询
        {
            next_ = 0;
        }
    }

    return loop;
}



std::vector<EventLoop *> EventLoopThreadPool::getAllLoops()
{
    if (loops_.empty())
    {
        return std::vector<EventLoop *>(1, baseLoop_); // 单线程模式：返回包含 baseLoop_ 的列表
    }
    else
    {
        return loops_;
    }
}