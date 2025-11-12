#pragma once

#include <unistd.h>
#include <sys/syscall.h>

namespace CurrentThread {
    // __thread 用于声明线程本地存储变量
    // 声明变量为外部定义（实际定义在 .cpp 文件中），避免头文件重复定义
    extern __thread int t_cachedTid; // 保存tid缓存 因为系统调用非常耗时 拿到tid后将其保存

    void cacheTid();

    inline int tid() { // 内联函数只在当前文件中起作用 
        // __builtin_expect 是一种底层优化 此语句意思是如果还未获取tid 进入if 通过cacheTid()系统调用获取tid
        if (__builtin_expect(t_cachedTid == 0, 0)) {
            cacheTid();
        }
        return t_cachedTid;
    }
}