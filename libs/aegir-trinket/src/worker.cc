/*
 * Trinket Worker pool implementation.
 */

#include <aegir/trinket/worker.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/unicode.h>
#include <sel4/sel4.h>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>

namespace aegir::trinket {

struct WorkerPool::ThreadData {
    WorkerPool* pool = nullptr;
    seL4_CPtr tcb = 0;
    std::thread std_thread;
};

WorkerPool::WorkerPool(int num_threads)
    : num_threads_(std::max(1, num_threads)) {
    threads_.resize(num_threads_);

    // Create seL4 TCBs for each worker thread
    // Note: In a real implementation, we'd use seL4 primitives
    // For now, use std::thread as a placeholder

    for (int i = 0; i < num_threads_; ++i) {
        threads_[i] = std::make_unique<ThreadData>();
        threads_[i]->pool = this;
        threads_[i]->std_thread = std::thread(&WorkerPool::run_loop, this);
    }
}

WorkerPool::~WorkerPool() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        shutting_down_ = true;
    }
    condition_.notify_all();

    for (auto& td : threads_) {
        if (td->std_thread.joinable()) {
            td->std_thread.join();
        }
    }
}

void WorkerPool::run_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            condition_.wait(lock, [this] {
                return !work_queue_.empty() || shutting_down_;
            });

            if (shutting_down_ && work_queue_.empty()) {
                return;
            }

            if (!work_queue_.empty()) {
                task = std::move(work_queue_.front());
                work_queue_.pop();
            }
        }

        if (task) {
            task();
        }
    }
}

void WorkerPool::post_to_main(std::function<void()>&& fn) {
    Application::instance()->post_event(std::move(fn));
}

void WorkerPool::wait_idle() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    condition_.wait(lock, [this] {
        return work_queue_.empty();
    });
}

} // namespace aegir::trinket