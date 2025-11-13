#pragma once

#include <functional>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>
 
#include "noncopyable.h"
#include "Timestamp.h"
#include "CurrentThread.h"

// 前置声明，避免循环依赖
class Channel;
class Poller;

/**
 * @brief 事件循环类 (Reactor 模式的核心)
 * 
 * 一个 EventLoop 实例代表一个独立的事件循环线程。它负责：
 * 1. 通过 Poller (I/O 多路复用器) 等待 I/O 事件（如 socket 可读、可写）。
 * 2. 当事件发生时，调用相应 Channel 的回调函数进行处理。
 * 3. 执行用户通过 runInLoop/queueInLoop 投递的任务（Functor）。
 * 
 * 核心特性：
 * - 每个 EventLoop 对象绑定一个线程，通过 threadId_ 标识。
 * - 线程安全的任务队列 (pendingFunctors_)，允许跨线程投递任务。
 * - 使用 wakeupFd_ 实现高效的跨线程唤醒。
 * 
 * 注意：
 * - 一个线程只能拥有一个 EventLoop 对象。
 * - EventLoop 的生命周期通常由其所属的线程（如 TcpServer 的主循环线程）管理。
 */
class EventLoop : noncopyable
{
public:
    // 定义任务回调类型
    using Functor = std::function<void()>;

    /**
     * @brief 构造函数
     * 
     * 在构造时，会：
     * 1. 记录当前线程 ID (threadId_)。
     * 2. 创建 Poller 对象 (epoll/poll 的封装)。
     * 3. 创建 wakeupFd_ (通过 eventfd) 并封装成 wakeupChannel_。
     * 4. 将 wakeupChannel_ 的读事件回调设置为 handleRead，用于唤醒事件循环。
     */
    EventLoop();
    

    /**
     * @brief 析构函数
     * 
     * 确保在析构时事件循环已经停止 (quit_ 为 true)。
     */
    ~EventLoop();

    /**
     * @brief 启动事件循环的主循环
     * 
     * 这是 EventLoop 的核心方法，调用后会进入一个无限循环，直到 quit() 被调用。
     * 循环逻辑：
     * 1. 调用 Poller::poll() 阻塞等待事件，超时时间由 kPollTimeMs 指定。
     * 2. poll() 返回后，获取就绪的 Channel 列表 (activeChannels_)。
     * 3. 遍历 activeChannels_，调用每个 Channel 的 handleEvent() 方法处理事件。
     * 4. 执行 doPendingFunctors()，处理在此期间被投递到队列中的任务。
     * 
     * 注意：此方法会阻塞调用线程，通常在一个线程的主函数中调用。
     */
    void loop();

    /**
     * @brief 安全地退出事件循环
     * 
     * 设置 quit_ 标志为 true。如果调用此方法的线程不是 EventLoop 所属的线程，
     * 必须唤醒事件循环线程，使其从 poll() 的阻塞中返回，以便检查 quit_ 标志并退出。
     */
    void quit();

    /**
     * @brief 获取最近一次 poll() 返回的时间戳
     * @return Timestamp 最近一次 I/O 事件就绪的时间
     */
    Timestamp pollReturnTime() const;

    /**
     * @brief 在 EventLoop 所属的线程中执行一个任务
     * 
     * 如果调用此方法的线程就是 EventLoop 所属的线程，则立即执行任务 cb。
     * 否则，将任务 cb 投递到任务队列 pendingFunctors_ 中，并唤醒事件循环线程以尽快执行。
     * 
     * @param cb 要执行的任务回调
     */
    void runInLoop(Functor cb);

    /**
     * @brief 将一个任务投递到 EventLoop 的任务队列中，待后续执行
     * 
     * 此方法是线程安全的。它会将任务 cb 加入 pendingFunctors_，
     * 并根据情况决定是否需要唤醒事件循环线程。
     * 唤醒的条件是：
     * 1. 调用线程不是 EventLoop 所属线程。
     * 2. 事件循环正在执行任务队列 (callingPendingFunctors_ 为 true)，以避免新任务被延迟到下一轮 poll。
     * 
     * @param cb 要投递的任务回调
     */
    void queueInLoop(Functor cb);

    /**
     * @brief 唤醒 EventLoop 线程
     * 
     * 通过向 wakeupFd_ 写入一个字节的数据，使阻塞在 poll() 上的 EventLoop 线程立即返回。
     * 这是实现跨线程唤醒 EventLoop 的标准方法。
     */
    void wakeup();

    /**
     * @brief 更新 Channel 的事件注册
     * 
     * 此方法是 Channel 和 Poller 之间的桥梁。当 Channel 的感兴趣事件发生变化时，
     * 它会调用此方法通知 Poller 更新其内部的数据结构 (如 epoll_ctl)。
     * 
     * @param channel 需要更新的 Channel 对象
     */
    void updateChannel(Channel *channel);

    /**
     * @brief 将 Channel 从 Poller 的监听列表中移除
     * 
     * 当一个连接关闭时，TcpConnection 会调用此方法将其对应的 Channel 从 EventLoop 中注销。
     * 
     * @param channel 需要移除的 Channel 对象
     */
    void removeChannel(Channel *channel);

    /**
     * @brief 检查 EventLoop 是否正在监听某个特定的 Channel
     * @param channel 要检查的 Channel 对象
     * @return true 如果正在监听，false 否则
     */
    bool hasChannel(Channel *channel);

    /**
     * @brief 检查当前调用线程是否是 EventLoop 所属的线程
     * @return true 如果是，false 否则
     */
    bool isInLoopThread() const;

private:
    /**
     * @brief wakeupFd_ 的读事件回调函数
     * 
     * 当其他线程调用 wakeup() 向 wakeupFd_ 写入数据时，此回调会被触发。
     * 它的主要工作是从 wakeupFd_ 中读取数据（通常是 8 字节），以清空内核缓冲区，
     * 使 wakeupFd_ 可以再次用于唤醒。它本身不执行任何业务逻辑。
     */
    void handleRead();

    /**
     * @brief 执行 pendingFunctors_ 队列中的所有任务
     * 
     * 为了避免在执行任务的过程中，任务队列被反复加锁（特别是当任务本身又调用 queueInLoop 时），
     * 此函数采用了“ swap and dispatch ”的策略：
     * 1. 用一个局部向量 functors 与 pendingFunctors_ 进行交换。
     * 2. 解锁，这样其他线程可以继续向 pendingFunctors_ 中添加任务。
     * 3. 遍历局部向量 functors 并执行其中的所有任务。
     * 
     * callingPendingFunctors_ 标志在任务执行期间被设为 true，用于 queueInLoop 判断是否需要唤醒。
     */
    void doPendingFunctors();

    // --- 成员变量 ---

    /// @brief 标记事件循环是否正在运行
    std::atomic_bool looping_;

    /// @brief 标记是否需要退出事件循环
    std::atomic_bool quit_;

    /// @brief 记录当前 EventLoop 所属的线程 ID
    const pid_t threadId_;

    /// @brief 记录最近一次 poll() 返回的时间戳
    Timestamp pollReturnTime_;

    /// @brief 指向 Poller 对象的智能指针，由 EventLoop 唯一拥有
    std::unique_ptr<Poller> poller_;

    /// @brief 用于跨线程唤醒的文件描述符，由 eventfd() 创建
    int wakeupFd_;

    /// @brief 封装 wakeupFd_ 的 Channel 对象
    std::unique_ptr<Channel> wakeupChannel_;

    /// @brief 存储每次 poll() 返回的就绪 Channel 列表
    ChannelList activeChannels_;

    /// @brief 保护 pendingFunctors_ 的互斥锁
    std::mutex mutex_;

    /// @brief 存储等待被执行的任务队列
    std::vector<Functor> pendingFunctors_;

    /// @brief 标记当前是否正在执行 pendingFunctors_ 中的任务
    /// 用于在 queueInLoop 中判断，当任务在执行期间又有新任务到来时，需要唤醒以避免延迟。
    std::atomic_bool callingPendingFunctors_;
};