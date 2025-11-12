// #include <iostream>
// #include "CurrentThread.h"

// int main() {
//     std::cout << CurrentThread::tid() << std::endl;
//     while (1);
//     return 0;
// }


#include "Thread.h"
#include <iostream>
#include <chrono>

void my_task() {
    std::cout << "Thread " << CurrentThread::tid() << " is running." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "Thread " << CurrentThread::tid() << " finished." << std::endl;
}

int main() {
    Thread t1(my_task, "MyWorkerThread");
    
    std::cout << "Before start, Thread ID: " << t1.tid() << std::endl; // 可能输出 0，也可能因为未同步而不确定，但在本实现中，start()会等待，所以这里没问题？不，这里在start()之前调用，tid_还是0。
    
    t1.start();
    
    std::cout << "After start, Thread ID: " << t1.tid() << std::endl; // 一定能正确输出新线程的TID
    
    t1.join();
    
    std::cout << "Thread joined." << std::endl;
    
    return 0;
}