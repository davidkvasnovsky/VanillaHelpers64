// ThreadPool.cpp - Priority-based thread pool implementation.
// Part of the TextureServer64 server component.

#include "ThreadPool.h"

namespace TexServer {

ThreadPool::ThreadPool(uint32_t num_threads) {
    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) {
            num_threads = 2; // safe fallback
        }
    }

    workers_.reserve(num_threads);
    for (uint32_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back(&ThreadPool::WorkerMain, this);
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> const lock(mutex_);
        shutdown_ = true;
        paused_ = false; // unblock any paused workers
    }
    cv_work_.notify_all();

    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

void ThreadPool::Submit(std::function<void()> task, uint8_t priority) {
    {
        std::lock_guard<std::mutex> const lock(mutex_);
        queue_.push(PriTask{.priority=priority, .sequence=sequence_++, .func=std::move(task)});
    }
    cv_work_.notify_one();
}

void ThreadPool::WaitIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_idle_.wait(lock, [this] { return queue_.empty() && in_flight_ == 0; });
}

void ThreadPool::Pause() {
    std::lock_guard<std::mutex> const lock(mutex_);
    paused_ = true;
}

void ThreadPool::Resume() {
    {
        std::lock_guard<std::mutex> const lock(mutex_);
        paused_ = false;
    }
    cv_work_.notify_all();
}

auto ThreadPool::WorkerCount() const -> uint32_t {
    return static_cast<uint32_t>(workers_.size());
}

void ThreadPool::WorkerMain() {
    for (;;) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_work_.wait(lock, [this] { return shutdown_ || (!paused_ && !queue_.empty()); });

            if (shutdown_ && queue_.empty()) {
                return;
            }

            // If paused_ became true again after wakeup but before we
            // grabbed the lock, loop back and wait.
            if (paused_) {
                continue;
            }

            task = std::move(const_cast<PriTask&>(queue_.top()).func);
            queue_.pop();
            ++in_flight_;
        }

        task();

        {
            std::lock_guard<std::mutex> const lock(mutex_);
            --in_flight_;
        }
        cv_idle_.notify_all();
    }
}

} // namespace TexServer
