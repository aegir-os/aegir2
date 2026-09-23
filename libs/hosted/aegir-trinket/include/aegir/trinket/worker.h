/*
 * Trinket Worker pool.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * seL4-native thread pool for background work.
 * No pthreads dependency.
 */

#ifndef AEGIR_TRINKET_WORKER_H
#define AEGIR_TRINKET_WORKER_H

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <queue>
#include <thread>

namespace aegir::trinket {

// Forward declare seL4 types
struct seL4_TCB;
struct seL4_Notification;

class WorkerPool {
public:
    explicit WorkerPool(int num_threads = 2);
    ~WorkerPool();

    // Non-copyable, movable
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    WorkerPool(WorkerPool&&) noexcept = default;
    WorkerPool& operator=(WorkerPool&&) noexcept = default;

    // Submit work to thread pool
    // Returns a future that will be fulfilled on the worker thread
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>> {
        using Result = std::invoke_result_t<F, Args...>;
        using Packaged = std::packaged_task<Result()>;

        auto task = std::make_shared<Packaged>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        auto future = task->get_future();

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            work_queue_.emplace([task]() { (*task)(); });
        }
        condition_.notify_one();
        return future;
    }

    // Post completion back to main thread (call from worker)
    static void post_to_main(std::function<void()>&& fn);

    // Wait for all pending work (for shutdown)
    void wait_idle();

    int num_threads() const { return num_threads_; }

private:
    struct ThreadData;
    friend struct ThreadData;

    static void thread_entry(void* arg);
    void run_loop();

    int num_threads_;
    std::vector<std::unique_ptr<ThreadData>> threads_;
    std::queue<std::function<void()>> work_queue_;
    std::mutex queue_mutex_;
    std::condition_variable condition_;
    bool shutting_down_ = false;
};

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_WORKER_H