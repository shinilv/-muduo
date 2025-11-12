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