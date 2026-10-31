#pragma once

#include <cstddef>
#include <functional>

namespace axk::app {

struct SourceVerificationCounts {
    std::size_t tree_opens{};
    std::size_t file_checks{};
    std::function<void()> on_tree_open;
    std::function<void()> on_file_check;
};

class SourceVerificationObservation final {
  public:
    explicit SourceVerificationObservation(SourceVerificationCounts &counts) : previous_(current_) {
        current_ = &counts;
    }
    ~SourceVerificationObservation() { current_ = previous_; }
    SourceVerificationObservation(const SourceVerificationObservation &) = delete;
    SourceVerificationObservation &operator=(const SourceVerificationObservation &) = delete;
    static void tree_open() {
        if (current_) {
            ++current_->tree_opens;
            if (current_->on_tree_open)
                current_->on_tree_open();
        }
    }
    static void file_check() {
        if (current_) {
            ++current_->file_checks;
            if (current_->on_file_check)
                current_->on_file_check();
        }
    }

  private:
    inline static thread_local SourceVerificationCounts *current_{};
    SourceVerificationCounts *previous_;
};

} // namespace axk::app
