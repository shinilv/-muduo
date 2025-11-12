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