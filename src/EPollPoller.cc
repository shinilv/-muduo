#include "EPollPoller.h"
#include "Logger.h"
#include "Channel.h"

#include <errno.h>
#include <unistd.h>
#include <string.h>

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

