#include "EPollPoller.h"
#include "Logger.h"
#include "Channel.h"

#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

const int kNew = -1;    // 某个channel还没添加至Poller          // channel的成员index_初始化为-1
const int kAdded = 1;   // 某个channel已经添加至Poller
const int kDeleted = 2; // 某个channel已经从Poller删除

EPollPoller::EPollPoller(EventLoop *loop) 
    :Poller(loop)
    ,epollfd_(::epoll_create(EPOLL_CLOEXEC))
    ,events_(kInitEventListSize)  // std::vector<epoll_event>(16)
{
    if (epollfd_ < 0) {
        LOG_FATAL("epoll_create error:%d\n", errno);
    }
}
EPollPoller::~EPollPoller() {
    ::close(epollfd_);
}

// 监听
Timestamp EPollPoller::poll(int timeoutMs, ChannelList *activeChannels) {
    // 由于频繁调用poll 实际上应该用LOG_DEBUG输出日志更为合理 当遇到并发场景 关闭DEBUG日志提升效率
    LOG_INFO("func=%s => fd total count:%lu\n", __FUNCTION__, channels_.size());

    int numEvents = ::epoll_wait(epollfd_, &*events_.begin(), static_cast<int>(events_.size()), timeoutMs);
    int saveErrno = errno;
    // 返回事件发生的精准时间
    Timestamp now(Timestamp::now());

    if (numEvents > 0) {
        LOG_INFO("%d events happend\n", numEvents); // LOG_DEBUG最合理
        fillActiveChannels(numEvents, activeChannels);
        if (numEvents == events_.size()) {
            // 扩容操作
            events_.resize(events_.size() * 2);
        }
    } else if (numEvents == 0) {
        LOG_DEBUG("%s timeout!\n", __FUNCTION__);
    } else {
        if (saveErrno != EINTR) {
            errno = saveErrno;
            LOG_ERROR("EPollPoller::poll() error!");
        }
    }
    return now;
}
// 重写基类方法，更新 Channel 在 epoll 中的事件监控状态
void EPollPoller::updateChannel(Channel *channel) {
    const int index = channel->index();
    LOG_INFO("func=%s => fd=%d events=%d index=%d\n", __FUNCTION__, channel->fd(), channel->events(), index);

    if (index == kNew || index == kDeleted) {
        if (index == kNew) {
            int fd = channel->fd();
            channels_[fd] = channel;
        } else { // index == kDeleted
        } 
        channel->set_index(kAdded); // 将状态设置为“已添加”
        update(EPOLL_CTL_ADD, channel); // 调用底层update函数，执行epoll_ctl ADD操作
    } else {
        // channel已经在Poller中注册过了
        int fd = channel->fd();
        if (channel->isNoneEvent()) {
            update(EPOLL_CTL_DEL, channel);
            channel->set_index(kDeleted);
        }
        else {
            update(EPOLL_CTL_MOD, channel);
        }
    }
}
// 重写基类方法，将 Channel 从 epoll 监控中移除
void EPollPoller::removeChannel(Channel *channel) {
    int fd = channel->fd();
    channels_.erase(fd);

    LOG_INFO("func=%s => fd=%d\n", __FUNCTION__, fd);

    int index = channel->index();
    if (index == kAdded)
    {
        update(EPOLL_CTL_DEL, channel);
    }
    channel->set_index(kNew);
}


// 核心辅助方法：将 epoll_wait 返回的就绪事件填充到 activeChannels（传出参数）
// 填写活跃的连接
void EPollPoller::fillActiveChannels(int numEvents, ChannelList *activeChannels) const
{
    // 遍历所有epoll返回的就绪事件
    for (int i = 0; i < numEvents; ++i)
    {
        // 1. 从epoll_event中获取Channel指针
        // events_[i] 是一个 struct epoll_event
        // events_[i].data.ptr 是我们在调用epoll_ctl时，通过 event.data.ptr 字段存入的 Channel*
        // 这里是将其强制转换回 Channel* 类型
        Channel *channel = static_cast<Channel *>(events_[i].data.ptr);
        
        // 2. 将epoll返回的具体事件类型设置到Channel对象中
        // events_[i].events 是一个整数，代表了发生的事件类型（如 EPOLLIN, EPOLLOUT, EPOLLERR）
        // channel->set_revents(...) 将这个事件类型存入Channel的 revents_ 成员变量
        // 这个 revents_ 就是Channel的 handleEvent() 方法判断应该执行哪个回调的依据
        channel->set_revents(events_[i].events);
        
        // 3. 将就绪的Channel添加到输出列表中
        // activeChannels 是一个由 EventLoop 传入的空列表
        // push_back(channel) 将当前这个发生了事件的Channel指针添加到列表末尾
        activeChannels->push_back(channel); // EventLoop就拿到了它的Poller给它返回的所有发生事件的channel列表了
    }
}

// 底层辅助方法：调用 epoll_ctl 执行 ADD/MOD/DEL 操作，更新 epoll 内部状态
void EPollPoller::update(int operation, Channel *channel) {
    epoll_event event; 
    ::memset(&event, 0, sizeof(event));

    int fd = channel->fd();

    event.events = channel->events();
    event.data.fd = fd;
    //这是最关键的一步！将 Channel 对象的指针存入 event.data.ptr 字段。
    // 当这个 fd 上有事件发生时，epoll_wait 会返回这个 event 结构体，
    // 我们就可以通过 event.data.ptr 快速地找到对应的 Channel 对象，
    // 而无需再通过 fd 去 channels_ 映射表中查找。这是 Channel 和 epoll_event 之间的直接桥梁。
    event.data.ptr = channel;

    if (::epoll_ctl(epollfd_, operation, fd, &event) < 0) {
        if (operation == EPOLL_CTL_DEL) {
            LOG_ERROR("epoll_ctl del error:%d\n", errno);
        } else {
            LOG_FATAL("epoll_ctl add/mod error:%d\n", errno);
        }
    }
}