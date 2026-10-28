// include/lis/thread_pool.h
#pragma once
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace lis {

// 有界线程池：停止后不再入队（提交线程直接执行）；队列满时由提交线程自己执行（CallerRuns，对齐 HAPI CustomThreading）。
class ThreadPool {
public:
    ThreadPool(size_t workers, size_t max_queue = 256);
    ~ThreadPool();
    void submit(std::function<void()> task);
    void stop();

private:
    void loop();
    std::vector<std::thread> threads_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mu_;
    std::condition_variable cv_;
    size_t max_queue_;
    bool stopping_ = false;
};

}  // namespace lis
