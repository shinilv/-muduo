#include <string.h>

#include "InetAddress.h"

InetAddress::InetAddress(uint16_t port, std::string ip) {
    ::memset(&addr_, 0, sizeof(addr_));  // 初始化地址结构体，将所有字节置0
    addr_.sin_family = AF_INET;  // 设置地址族为IPv4
    addr_.sin_port = ::htons(port);  // 将端口号从主机字节序转换为网络字节序
    addr_.sin_addr.s_addr = ::inet_addr(ip.c_str());  // 将IP字符串转换为网络字节序的32位整数
}

// 将网络字节序的 32 位 IP 地址转换为点分十进制的字符串形式，方便人类阅读。
std::string InetAddress::toIp() const {
    char buf[64] = {0};
    ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof buf);
    return buf;
}

// 将 IP 地址和端口号组合成 “ip:port” 的格式，例如 “127.0.0.1:8080”，在日志输出或调试时非常有用。
std::string InetAddress::toIpPort() const {
    char buf[64] = {0};
    ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof buf);
    size_t end = ::strlen(buf);
    uint16_t port = ::ntohs(addr_.sin_port);
    sprintf(buf+end, ":%u", port);
    return buf;
}

//返回主机字节序的端口号，方便在程序中使用。
uint16_t InetAddress::toPort() const {
    return ::ntohs(addr_.sin_port);
}

// #include <iostream>
// int main() {
//     InetAddress addr(8080);
//     std::cout << addr.toIpPort() << std::endl;
// }