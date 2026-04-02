#pragma once
// ThreadPool.h - Priority-based thread pool for texture decoding.
// Part of the TextureServer64 server component.
// Requirements: C++17 standard library only (no platform-specific APIs).

#include <cstdint>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace TexServer {

class ThreadPool {
public:
    // Construct a pool with the given number of worker threads.
    // If num_threads == 0, uses std::thread::hardware_concurrency()
    // (with a fallback of 2 if that returns 0).
    explicit ThreadPool(uint32_t num_threads = 0);

    // Destructor: signals shutdown, waits for all in-flight tasks to finish.
    ~ThreadPool();

    // Non-copyable, non-movable.
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Submit a fire-and-forget task.
    // priority: 0 = highest, 255 = lowest. Default 128 (normal).
    void Submit(std::function<void()> task, uint8_t priority = 128);

    // Submit a task that returns a value, accessible via std::future.
    template<typename F>
    auto SubmitWithResult(F&& func, uint8_t priority = 128)
        -> std::future<decltype(func())>;

    // Block until all submitted tasks have finished executing.
    void WaitIdle();

    // Pause consumption of new tasks (workers finish their current task
    // but do not pick up new ones until Resume() is called).
    void Pause();

    // Resume task consumption after a Pause().
    void Resume();

    // Number of worker threads.
    uint32_t WorkerCount() const;

private:
    // Internal prioritised task wrapper.
    struct PriTask {
        uint8_t                priority;  // 0 = highest
        uint64_t               sequence;  // monotonic, for FIFO tiebreak
        std::function<void()>  func;

        // For std::priority_queue (max-heap), we invert the comparison
        // so that *lower* priority values and *lower* sequence numbers
        // are dequeued first.
        bool operator<(const PriTask& rhs) const {
            if (priority != rhs.priority)
                return priority > rhs.priority;   // lower value = higher pri
            return sequence > rhs.sequence;        // earlier = higher pri
        }
    };

    void WorkerMain();

    std::vector<std::thread>                  workers_;
    std::priority_queue<PriTask>              queue_;
    mutable std::mutex                        mutex_;
    std::condition_variable                   cv_work_;    // workers wait here
    std::condition_variable                   cv_idle_;    // WaitIdle waits here

    uint64_t sequence_  = 0;       // monotonically increasing task counter
    uint32_t in_flight_ = 0;       // tasks currently being executed
    bool     shutdown_  = false;
    bool     paused_    = false;
};

// ── Template implementation ────────────────────────────────────────────────

template<typename F>
auto ThreadPool::SubmitWithResult(F&& func, uint8_t priority)
    -> std::future<decltype(func())>
{
    using ReturnType = decltype(func());

    auto promise = std::make_shared<std::promise<ReturnType>>();
    std::future<ReturnType> future = promise->get_future();

    Submit(
        [p = std::move(promise), f = std::forward<F>(func)]() mutable {
            try {
                if constexpr (std::is_void_v<ReturnType>) {
                    f();
                    p->set_value();
                } else {
                    p->set_value(f());
                }
            } catch (...) {
                p->set_exception(std::current_exception());
            }
        },
        priority);

    return future;
}

} // namespace TexServer
