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