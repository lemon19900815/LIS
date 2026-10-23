// src/thread_pool.cpp
#include <lis/thread_pool.h>

namespace lis {

ThreadPool::ThreadPool(size_t workers, size_t max_queue) : max_queue_(max_queue) {
    threads_.reserve(workers);
    for (size_t i = 0; i < workers; ++i) threads_.emplace_back([this] { loop(); });
}

ThreadPool::~ThreadPool() { stop(); }

void ThreadPool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopping_ || tasks_.size() >= max_queue_) {
            // 不入队：stopping_ 或队列满，都由调用者执行
        } else {
            tasks_.push(std::move(task));
            cv_.notify_one();
            return;
        }
    }
    if (task) task();   // CallerRuns
}

void ThreadPool::loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}

void ThreadPool::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopping_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) if (t.joinable()) t.join();
}

}  // namespace lis
