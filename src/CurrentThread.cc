#include "CurrentThread.h"
#include <iostream>

namespace CurrentThread
{
    __thread int t_cachedTid = 0;

    void cacheTid() {
        // 首次获取，使用系统调用
        if (t_cachedTid == 0) {
            t_cachedTid = static_cast<pid_t>(::syscall(SYS_gettid));
        }
    }
}

