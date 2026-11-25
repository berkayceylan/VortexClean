#include "ThreadPool.hpp"

#include <algorithm>
#include <pthread.h>
#include <pthread/qos.h>
#include <utility>

// CoreFoundation, Security and Spotlight calls made by tasks autorelease objects
// internally. A long-lived worker has no run loop to drain them, so every task
// runs inside its own autorelease pool. These two functions are the stable
// libobjc entry points that `@autoreleasepool` compiles down to; using them
// keeps this file plain C++.
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void* pool);

namespace vortex {

namespace {

class AutoreleaseScope {
public:
    AutoreleaseScope() noexcept : pool_(objc_autoreleasePoolPush()) {}
    ~AutoreleaseScope() { objc_autoreleasePoolPop(pool_); }
    AutoreleaseScope(const AutoreleaseScope&) = delete;
    AutoreleaseScope& operator=(const AutoreleaseScope&) = delete;

private:
    void* pool_;
};

}  // namespace

ThreadPool::ThreadPool(std::size_t threads, Priority priority, std::string name)
    : priority_(priority), name_(std::move(name)) {
    if (threads == 0) {
        const unsigned hardware = std::thread::hardware_concurrency();
        threads = std::clamp<std::size_t>(hardware == 0 ? 4 : hardware, 2, 8);
    }
    workers_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        workers_.emplace_back([this, i] { workerLoop(i); });
    }
}

ThreadPool::~ThreadPool() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    // Workers drain whatever is still queued before they exit. Nothing is ever
    // dropped, because a dropped task would leave its TaskGroup waiting forever;
    // owners cancel their tokens first so the remaining tasks return at once.
    for (std::thread& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

void ThreadPool::submit(std::function<void()> task) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!stopping_) {
            queue_.push_back(std::move(task));
            task = nullptr;
        }
    }
    if (task) {
        // The pool is shutting down: run on the caller's thread instead of dropping the work.
        task();
        return;
    }
    wake_.notify_one();
}

bool ThreadPool::tryRunOne() {
    std::function<void()> task;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        task = std::move(queue_.front());
        queue_.pop_front();
    }
    const AutoreleaseScope pool;
    task();
    return true;
}

void ThreadPool::workerLoop(std::size_t index) {
    const std::string threadName = name_ + "." + std::to_string(index);
    pthread_setname_np(threadName.c_str());
    pthread_set_qos_class_self_np(
        priority_ == Priority::UserInitiated ? QOS_CLASS_USER_INITIATED : QOS_CLASS_UTILITY, 0);

    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) return;  // only reachable once stopping_ is set
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        const AutoreleaseScope pool;
        task();
    }
}

TaskGroup::~TaskGroup() {
    waitNoThrow();
}

void TaskGroup::run(std::function<void()> task) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        ++pending_;
    }
    pool_.submit([this, task = std::move(task)]() mutable {
        std::exception_ptr error;
        try {
            task();
        } catch (...) {
            error = std::current_exception();
        }
        // Destroy the callable (and whatever it captured) before signalling
        // completion, so nothing it owns outlives a returning wait().
        task = nullptr;
        // Notify while holding the lock: the waiter cannot wake, return and
        // destroy this group until the lock is released, so `done_` is alive
        // for the whole notify call.
        const std::lock_guard<std::mutex> lock(mutex_);
        if (error && !firstError_) firstError_ = error;
        --pending_;
        done_.notify_all();
    });
}

void TaskGroup::waitNoThrow() noexcept {
    for (;;) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (pending_ == 0) return;
        }
        // Help the pool instead of sleeping. A waiter only blocks once the queue
        // is empty, at which point every outstanding task of this group is
        // already running on some thread — so waiting from inside a pool task
        // (nested fork/join) can never deadlock, and no timed polling is needed.
        bool ranOne = false;
        try {
            ranOne = pool_.tryRunOne();
        } catch (...) {
            ranOne = true;  // tasks submitted through a TaskGroup never throw; defensive only
        }
        if (ranOne) continue;
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [this] { return pending_ == 0; });
        return;
    }
}

void TaskGroup::wait() {
    waitNoThrow();
    std::exception_ptr error;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::swap(error, firstError_);
    }
    if (error) std::rethrow_exception(error);
}

}  // namespace vortex
