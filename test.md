# 项目说明

# 环境依赖
确保系统已安装以下基础依赖：
CMake（建议 3.10 及以上版本）
GCC/G++（支持 C++11 及以上标准）
make 工具
基础的 Linux 开发环境（如 libc6-dev 等）

编译构建

git clone https://github.com/shinilv/-muduo.git
cd -muduo

# 创建build目录
mkdir build
# 进入build目录
cd build
# 执行CMake配置（默认使用Release模式）
cmake ..
# 编译项目（可加-j参数开启多线程编译，如make -j4）
make

# 回到项目根目录（若当前在build目录）
cd ../example
# 启动echo服务端
./server

# 项目测试说明

使用py脚本进行压力测试
~~~
import socket
import threading
import time
import argparse
from datetime import datetime
from collections import defaultdict

# 全局统计变量
stats = {
    "total_requests": 0,    # 总请求数
    "success_requests": 0,  # 成功请求数
    "failed_requests": 0,   # 失败请求数
    "total_latency": 0.0,   # 总延迟（秒）
    "max_latency": 0.0,     # 最大延迟（秒）
    "min_latency": float('inf'),  # 最小延迟（秒）
    "lock": threading.Lock()  # 线程安全锁
}


def recv_exact(sock, expected_len):
    """
    从 socket 中精确读取 expected_len 字节，直到读满或出错/超时。
    返回 bytes；如果中途失败则抛异常。
    """
    chunks = []
    bytes_recd = 0
    while bytes_recd < expected_len:
        chunk = sock.recv(expected_len - bytes_recd)
        if not chunk:
            # 连接被对端关闭
            raise ConnectionError("socket closed before receiving expected data")
        chunks.append(chunk)
        bytes_recd += len(chunk)
    return b"".join(chunks)


def client_worker(server_ip, server_port, msg, msg_count, timeout=5):
    """
    单个客户端线程逻辑：连接服务器，发送指定数量的消息并接收回显
    :param server_ip: 服务器IP
    :param server_port: 服务器端口
    :param msg: 要发送的消息内容
    :param msg_count: 每个客户端发送的消息数
    :param timeout: 连接/读写超时时间（秒）
    """
    global stats
    sock = None
    try:
        # 创建TCP连接
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(timeout)  # 设置超时
        # 连接服务器
        sock.connect((server_ip, server_port))
        
        for _ in range(msg_count):
            start_time = time.time()
            try:
                # 发送消息
                sock.sendall(msg.encode('utf-8'))
                # 接收回显（按消息长度接收，避免粘包 / 半包）
                expected_len = len(msg.encode('utf-8'))
                recv_data = recv_exact(sock, expected_len)
                
                # 统计结果
                latency = time.time() - start_time
                with stats["lock"]:
                    stats["total_requests"] += 1
                    if recv_data.decode('utf-8') == msg:
                        stats["success_requests"] += 1
                        stats["total_latency"] += latency
                        stats["max_latency"] = max(stats["max_latency"], latency)
                        stats["min_latency"] = min(stats["min_latency"], latency)
                    else:
                        stats["failed_requests"] += 1
            except Exception as e:
                with stats["lock"]:
                    stats["total_requests"] += 1
                    stats["failed_requests"] += 1
                # print(f"发送/接收失败: {e}")
    except Exception as e:
        with stats["lock"]:
            # 连接失败则所有请求算失败
            stats["total_requests"] += msg_count
            stats["failed_requests"] += msg_count
        # print(f"客户端连接失败: {e}")
    finally:
        if sock:
            sock.close()

def print_stats(start_time, end_time):
    """打印压测统计结果"""
    total_time = end_time - start_time
    qps = stats["success_requests"] / total_time if total_time > 0 else 0
    avg_latency = stats["total_latency"] / stats["success_requests"] if stats["success_requests"] > 0 else 0
    
    print("\n" + "="*50)
    print(f"压测结束 | 时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print("="*50)
    print(f"总耗时: {total_time:.2f} 秒")
    print(f"总请求数: {stats['total_requests']}")
    print(f"成功请求数: {stats['success_requests']}")
    print(f"失败请求数: {stats['failed_requests']}")
    print(f"成功率: {stats['success_requests']/stats['total_requests']*100:.2f}%" if stats['total_requests'] > 0 else "成功率: 0%")
    print(f"QPS (每秒成功请求数): {qps:.2f}")
    print(f"平均延迟: {avg_latency*1000:.2f} 毫秒")
    print(f"最大延迟: {stats['max_latency']*1000:.2f} 毫秒")
    print(f"最小延迟: {stats['min_latency']*1000:.2f} 毫秒" if stats['min_latency'] != float('inf') else "最小延迟: 0 毫秒")
    print("="*50)

def main():
    # 命令行参数解析
    parser = argparse.ArgumentParser(description='TCP Echo 服务器压力测试工具')
    parser.add_argument('--ip', default='127.0.0.1', help='服务器IP地址，默认127.0.0.1')
    parser.add_argument('--port', type=int, required=True, help='服务器端口（必填）')
    parser.add_argument('--concurrency', type=int, default=100, help='并发客户端数，默认100')
    parser.add_argument('--msgs-per-client', type=int, default=1000, help='每个客户端发送的消息数，默认1000')
    parser.add_argument('--msg', default='hello, muduo!\n', help='发送的消息内容，默认"hello, muduo!\n"')
    parser.add_argument('--timeout', type=int, default=5, help='连接/读写超时时间（秒），默认5')
    
    args = parser.parse_args()
    
    # 打印压测配置
    print("="*50)
    print(f"压测开始 | 时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print("="*50)
    print(f"服务器地址: {args.ip}:{args.port}")
    print(f"并发客户端数: {args.concurrency}")
    print(f"每个客户端发送消息数: {args.msgs_per_client}")
    print(f"单条消息内容: {repr(args.msg)}")
    print(f"超时时间: {args.timeout} 秒")
    print("="*50)
    
    # 记录开始时间
    start_time = time.time()
    
    # 创建并发客户端线程
    threads = []
    for i in range(args.concurrency):
        t = threading.Thread(
            target=client_worker,
            args=(args.ip, args.port, args.msg, args.msgs_per_client, args.timeout)
        )
        threads.append(t)
        t.start()
    
    # 等待所有线程结束
    for t in threads:
        t.join()
    
    # 记录结束时间并打印统计
    end_time = time.time()
    print_stats(start_time, end_time)

if __name__ == '__main__':
    main()
~~~

# 示例1：2000 并发 × 500 条/客户端 = 100万请求
python3 tcp_echo_bench.py --port 8080 --concurrency 2000 --msgs-per-client 500

# 示例2：1000 并发 × 1000 条/客户端 = 100万请求
python3 tcp_echo_bench.py --port 8080 --concurrency 1000 --msgs-per-client 1000

网络库性能看具体测试结果