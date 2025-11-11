#pragma once // 预处理指令，防止头文件重复包含

class noncopyable {
public:
    // 禁止这两类常见的拷贝操作
    noncopyable(const noncopyable &) = delete;
    noncopyable &operator = (const noncopyable &) = delete;

protected:
// 将构造函数声明为 protected（受保护的），意味着只有它的派生类（子类）才能访问。
// 这就防止了用户直接创建 noncopyable 
// 类的实例（因为在类外部无法调用 protected 的构造函数），符合其作为基类的设计初衷。
    noncopyable() = default;
    ~noncopyable() = default;
};