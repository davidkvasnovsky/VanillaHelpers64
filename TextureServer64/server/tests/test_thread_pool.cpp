// test_thread_pool.cpp - Tests for the ThreadPool class.
// Compile: cl /EHsc /std:c++17 /I..\src test_thread_pool.cpp ..\src\ThreadPool.cpp
// or:      g++ -std=c++17 -I../src -pthread test_thread_pool.cpp ../src/ThreadPool.cpp -o test_thread_pool

#include "ThreadPool.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

static int g_failures = 0;

#define TEST_ASSERT(cond, msg)                                                                                         \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__);                                      \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

#define TEST_ASSERT_EQ(a, b, msg)                                                                                      \
    do {                                                                                                               \
        if ((a) != (b)) {                                                                                              \
            std::fprintf(                                                                                              \
                stderr,                                                                                                \
                "FAIL: %s  (got %llu, expected %llu)"                                                                  \
                "  (%s:%d)\n",                                                                                         \
                msg,                                                                                                   \
                (unsigned long long)(a),                                                                               \
                (unsigned long long)(b),                                                                               \
                __FILE__,                                                                                              \
                __LINE__                                                                                               \
            );                                                                                                         \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

// ── test_basic_execution ───────────────────────────────────────────────
// Submit 100 tasks to a multi-threaded pool and verify all of them run.
static void test_basic_execution() {
    std::printf("test_basic_execution...\n");

    constexpr int TASK_COUNT = 100;
    std::atomic<int> counter{0};

    {
        TexServer::ThreadPool pool(4);
        for (int i = 0; i < TASK_COUNT; ++i) {
            pool.Submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
        }
        pool.WaitIdle();
        TEST_ASSERT_EQ(counter.load(), TASK_COUNT, "All 100 tasks executed");
    }

    // After destructor, counter must still be correct.
    TEST_ASSERT_EQ(counter.load(), TASK_COUNT, "Counter intact after pool destruction");
}

// ── test_future_result ─────────────────────────────────────────────────
// Submit a task that returns an int and verify the result via future.
static void test_future_result() {
    std::printf("test_future_result...\n");

    TexServer::ThreadPool pool(2);

    auto fut = pool.SubmitWithResult([] { return 6 * 7; });

    int const result = fut.get();
    TEST_ASSERT_EQ(result, 42, "Future returns correct value");

    // Also test void-returning SubmitWithResult.
    auto vfut = pool.SubmitWithResult([] {
        // no-op, just testing void specialisation
    });
    vfut.get(); // should not throw

    // Test exception propagation.
    auto efut = pool.SubmitWithResult([]() -> int { throw std::runtime_error("test error"); });

    bool caught = false;
    try {
        efut.get();
    } catch (const std::runtime_error& e) {
        caught = true;
    }
    TEST_ASSERT(caught, "Exception propagated through future");
}

// ── test_priority_ordering ─────────────────────────────────────────────
// Use a single-thread pool, pause it, enqueue tasks with different
// priorities, resume, and verify that higher-priority tasks ran first.
static void test_priority_ordering() {
    std::printf("test_priority_ordering...\n");

    TexServer::ThreadPool pool(1);
    pool.Pause();

    std::mutex order_mutex;
    std::vector<int> execution_order;

    // Enqueue tasks with varying priorities.
    // priority 255 = lowest, 0 = highest, 128 = medium.
    // We submit in order: low, medium, high, very-high.
    pool.Submit(
        [&] {
            std::lock_guard<std::mutex> const lk(order_mutex);
            execution_order.push_back(255);
        },
        255
    );

    pool.Submit(
        [&] {
            std::lock_guard<std::mutex> const lk(order_mutex);
            execution_order.push_back(128);
        },
        128
    );

    pool.Submit(
        [&] {
            std::lock_guard<std::mutex> const lk(order_mutex);
            execution_order.push_back(1);
        },
        1
    );

    pool.Submit(
        [&] {
            std::lock_guard<std::mutex> const lk(order_mutex);
            execution_order.push_back(0);
        },
        0
    );

    pool.Resume();
    pool.WaitIdle();

    TEST_ASSERT_EQ(execution_order.size(), 4U, "All priority tasks ran");

    // Expected order: 0, 1, 128, 255 (highest priority first).
    if (execution_order.size() == 4) {
        TEST_ASSERT_EQ(execution_order[0], 0, "First executed: priority 0");
        TEST_ASSERT_EQ(execution_order[1], 1, "Second executed: priority 1");
        TEST_ASSERT_EQ(execution_order[2], 128, "Third executed: priority 128");
        TEST_ASSERT_EQ(execution_order[3], 255, "Fourth executed: priority 255");
    }

    // Also verify FIFO within the same priority level.
    pool.Pause();
    execution_order.clear();

    pool.Submit(
        [&] {
            std::lock_guard<std::mutex> const lk(order_mutex);
            execution_order.push_back(1);
        },
        50
    );

    pool.Submit(
        [&] {
            std::lock_guard<std::mutex> const lk(order_mutex);
            execution_order.push_back(2);
        },
        50
    );

    pool.Submit(
        [&] {
            std::lock_guard<std::mutex> const lk(order_mutex);
            execution_order.push_back(3);
        },
        50
    );

    pool.Resume();
    pool.WaitIdle();

    TEST_ASSERT_EQ(execution_order.size(), 3U, "All same-priority tasks ran");
    if (execution_order.size() == 3) {
        TEST_ASSERT_EQ(execution_order[0], 1, "FIFO: first submitted runs first");
        TEST_ASSERT_EQ(execution_order[1], 2, "FIFO: second submitted runs second");
        TEST_ASSERT_EQ(execution_order[2], 3, "FIFO: third submitted runs third");
    }
}

// ── test_shutdown ──────────────────────────────────────────────────────
// Verify that the destructor waits for all submitted tasks to finish.
static void test_shutdown() {
    std::printf("test_shutdown...\n");

    std::atomic<int> counter{0};
    constexpr int TASK_COUNT = 50;

    {
        TexServer::ThreadPool pool(4);
        for (int i = 0; i < TASK_COUNT; ++i) {
            pool.Submit([&counter] {
                // Simulate some work.
                volatile int sink = 0;
                for (int j = 0; j < 10'000; ++j) {
                    sink = j;
                }
                (void)sink;
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }
        // Destructor runs here -- must wait for all tasks.
    }

    TEST_ASSERT_EQ(counter.load(), TASK_COUNT, "Destructor waited for all tasks");
}

// ── test_worker_count ──────────────────────────────────────────────────
static void test_worker_count() {
    std::printf("test_worker_count...\n");

    TexServer::ThreadPool const pool4(4);
    TEST_ASSERT_EQ(pool4.WorkerCount(), 4U, "WorkerCount == 4");

    TexServer::ThreadPool const pool1(1);
    TEST_ASSERT_EQ(pool1.WorkerCount(), 1U, "WorkerCount == 1");
}

// ── main ───────────────────────────────────────────────────────────────
auto main() -> int {
    std::printf("=== ThreadPool Tests ===\n");

    test_basic_execution();
    test_future_result();
    test_priority_ordering();
    test_shutdown();
    test_worker_count();

    if (g_failures == 0) {
        std::printf("\nAll tests PASSED.\n");
        return 0;
    }         std::fprintf(stderr, "\n%d test(s) FAILED.\n", g_failures);
        return 1;
   
}
