// VortexClean core — cooperative cancellation.
#pragma once

#include <atomic>
#include <memory>

namespace vortex {

/// A copyable handle onto a shared cancellation flag. Copies observe the same
/// flag, so the owner can hand a token to any number of worker threads and
/// abort all of them with a single `cancel()`. Checking the flag is a single
/// relaxed atomic load, cheap enough to do once per directory entry.
class CancellationToken {
public:
    CancellationToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

    void cancel() const noexcept { flag_->store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool isCancelled() const noexcept { return flag_->load(std::memory_order_relaxed); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

}  // namespace vortex
