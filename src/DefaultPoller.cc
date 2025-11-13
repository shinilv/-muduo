#include <stdlib.h>

#include "Poller.h"
#include "EPollPoller.h"

//由于只实现了 EPollPoller，所以这样实现
Poller *Poller::newDefaultPoller(EventLoop *loop)
{
    if (::getenv("MUDUO_USE_POLL"))
    {
        return nullptr; // 生成poll的实例
    }
    else
    {
        return new EPollPoller(loop); // 生成epoll的实例
    }
}