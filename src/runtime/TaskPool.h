#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace throne {
/// Small bounded pool for native simulation tasks; waiting callers help drain the queue.
class TaskPool {
public:
    /// Leaves room for the main (rendering) thread and the simulation driver.
    explicit TaskPool(unsigned count = simulationWorkers()) {
        workers_.reserve(count);
        try {
            for (unsigned i = 0; i < count; ++i) workers_.emplace_back([this] {
                for (;;) {
                    std::function<void()> work;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        ready_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                        if (stopping_ && queue_.empty()) return;
                        work = std::move(queue_.front());
                        queue_.pop_front();
                    }
                    work();
                }
            });
        } catch (...) {
            { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
            ready_.notify_all();
            for (auto& worker : workers_) worker.join();
            throw;
        }
    }
    /// Reserves two cores for rendering and the simulation driver.
    static unsigned simulationWorkers() {
        const auto cores = std::thread::hardware_concurrency();
        return cores > 2 ? (std::min)(cores - 2, 4u) : 0u;
    }
    /// Joins all workers after draining pending tasks.
    ~TaskPool() {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
        ready_.notify_all();
        for (auto& worker : workers_) worker.join();
    }
    /// Number of background workers, excluding helping callers.
    unsigned size() const { return static_cast<unsigned>(workers_.size()); }
    /// Enqueues work, returning its completion token.
    std::future<void> submit(std::function<void()> work) {
        auto task = std::make_shared<std::packaged_task<void()>>(std::move(work));
        auto future = task->get_future();
        { std::lock_guard<std::mutex> lock(mutex_); queue_.push_back([task] { (*task)(); }); }
        ready_.notify_one();
        return future;
    }
    /// Waits without deadlocking when a task waits for its own child tasks.
    void finish(std::future<void>& future) {
        while (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            std::function<void()> work;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!queue_.empty()) { work = std::move(queue_.front()); queue_.pop_front(); }
            }
            if (work) work();
            else future.wait_for(std::chrono::milliseconds(1));
        }
        future.get();
    }
private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};
}
