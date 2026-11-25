// VortexClean core — fixed-size worker pool with structured task groups.
//
// Idle workers block on a condition variable, so an idle pool costs zero CPU.
// Cancellation is cooperative: tasks capture a CancellationToken and return
// early; the pool itself never interrupts a running task.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vortex {

class ThreadPool {
public:
    enum class Priority { Utility, UserInitiated };

    /// `threads == 0` picks a size from the hardware concurrency (clamped to 2...8).
    explicit ThreadPool(std::size_t threads = 0, Priority priority = Priority::UserInitiated,
                        std::string name = "vortex.pool");
    /// Drains the queue and joins. No task is ever dropped (a dropped task would
    /// strand its TaskGroup); cancel the relevant tokens first for a fast shutdown.
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void submit(std::function<void()> task);

    /// Runs one queued task on the calling thread, if any is pending. Lets a
    /// thread that is waiting on a TaskGroup help drain the queue, which makes
    /// nested fork/join on the same pool deadlock-free.
    bool tryRunOne();

    [[nodiscard]] std::size_t threadCount() const noexcept { return workers_.size(); }

private:
    void workerLoop(std::size_t index);

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
    Priority priority_;
    std::string name_;
};

/// A batch of tasks that can be awaited together. The first exception thrown by
/// any task is captured and rethrown from wait().
class TaskGroup {
public:
    explicit TaskGroup(ThreadPool& pool) : pool_(pool) {}
    /// Blocks until every task has finished; exceptions are swallowed here (call wait() to observe them).
    ~TaskGroup();

    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;

    void run(std::function<void()> task);
    void wait();

private:
    void waitNoThrow() noexcept;

    ThreadPool& pool_;
    std::mutex mutex_;
    std::condition_variable done_;
    std::size_t pending_ = 0;
    std::exception_ptr firstError_;
};

}  // namespace vortex
