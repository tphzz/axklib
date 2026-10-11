#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace rawdisk {

class CancellationSource;

class CancellationToken {
  public:
    CancellationToken() noexcept = default;

    bool is_cancelled() const noexcept { return state_ && state_->load(std::memory_order_acquire); }

  private:
    explicit CancellationToken(std::shared_ptr<std::atomic_bool> state) noexcept : state_(std::move(state)) {}
    std::shared_ptr<std::atomic_bool> state_;
    friend class CancellationSource;
};

class CancellationSource {
  public:
    CancellationSource() : state_(std::make_shared<std::atomic_bool>(false)) {}

    CancellationToken token() const noexcept { return CancellationToken{state_}; }
    void cancel() const noexcept {
        if (state_)
            state_->store(true, std::memory_order_release);
    }

  private:
    std::shared_ptr<std::atomic_bool> state_;
};

} // namespace rawdisk
