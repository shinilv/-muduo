#pragma once

#include <iostream>
#include <string>

class Timestamp {
public:
    Timestamp();
    explicit Timestamp(int64_t microSecondsSinceEpoch);
    static Timestamp now();
    std::string toString() const; // const修饰，该函数只读

private:
    int64_t microSecondsSinceEpoch_; // 微妙级
};