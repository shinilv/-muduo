#pragma once

/**
 * 用户使用muduo编写服务器程序
 **/

#include <functional>
#include <string>
#include <memory>
#include <atomic>
#include <unordered_map>

#include "EventLoop.h"
#include "Acceptor.h"
#include "InetAddress.h"
#include "noncopyable.h"
#include "EventLoopThreadPool.h"
#include "Callbacks.h"
#include "TcpConnection.h"
#include "Buffer.h"

// 对外的服务器编程使用的类
class TcpServer
{
public:
    using ThreadInitCallback = std::function<void(EventLoop *)>;

    enum Option
    {
        kNoReusePort,//不允许重用本地端口
        kReusePort,//允许重用本地端口 
    };

    TcpServer(EventLoop *loop,
              const InetAddress &listenAddr,
              const std::string &nameArg,
              Option option = kNoReusePort);
    ~TcpServer();

    void setThreadInitCallback(const ThreadInitCallback &cb) { threadInitCallback_ = cb; }
    void setConnectionCallback(const ConnectionCallback &cb) { connectionCallback_ = cb; }
    void setMessageCallback(const MessageCallback &cb) { messageCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback &cb) { writeCompleteCallback_ = cb; }

    // 设置底层subloop的个数
    void setThreadNum(int numThreads);
    /**
     * 如果没有监听, 就启动服务器(监听).
     * 多次调用没有副作用.
     * 线程安全.
     */
    void start();

private:
    void newConnection(int sockfd, const InetAddress &peerAddr);
    void removeConnection(const TcpConnectionPtr &conn);
    void removeConnectionInLoop(const TcpConnectionPtr &conn);

    using ConnectionMap = std::unordered_map<std::string, TcpConnectionPtr>;

    EventLoop *loop_; // 主循环（Main Reactor)，用户传入，运行在主线程

    const std::string ipPort_; // 监听地址（IP:Port），如 "127.0.0.1:8080"
    const std::string name_; // 服务器名称，用于日志和调试

    // 封装监听 socket 和连接接收逻辑，有新连接时触发 newConnection 回调。
    std::unique_ptr<Acceptor> acceptor_; // 连接接收器，运行在主循环，负责监听接口，接受新连接

    // 子循环线程池，通过 setThreadNum 配置线程数，每个线程对应一个 EventLoop，负责处理已建立连接的 I/O 事件。
    std::shared_ptr<EventLoopThreadPool> threadPool_; // one loop per thread
    
    ConnectionCallback connectionCallback_;       //有新连接时的回调
    MessageCallback messageCallback_;             // 有读写事件发生时的回调
    WriteCompleteCallback writeCompleteCallback_; // 消息发送完成后的回调

    ThreadInitCallback threadInitCallback_; // loop线程初始化的回调
    int numThreads_;//线程池中线程的数量。
    std::atomic_int started_; // 原子变量，标记服务器是否已启动（避免重复启动）
    int nextConnId_; // 连接 ID 计数器，为每个新连接分配唯一 ID
    ConnectionMap connections_; // 保存所有的连接
};