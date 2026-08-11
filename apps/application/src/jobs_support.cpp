#include "jobs_support.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace axk::app::job_runtime_detail {

std::uint64_t timestamp_unix_ms() {
    const auto duration = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
}

std::uint64_t elapsed_ms(std::chrono::steady_clock::time_point start,
                         std::chrono::steady_clock::time_point end) noexcept {
    return static_cast<std::uint64_t>(
        std::max<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count(), 0));
}

std::string progress_phase_name(ProgressPhase phase) {
    switch (phase) {
    case ProgressPhase::opening:
        return "opening";
    case ProgressPhase::reading:
        return "reading";
    case ProgressPhase::resolving:
        return "resolving";
    case ProgressPhase::validating:
        return "validating";
    case ProgressPhase::exporting:
        return "exporting";
    case ProgressPhase::writing:
        return "writing";
    case ProgressPhase::allocating:
        return "allocating";
    case ProgressPhase::publishing:
        return "publishing";
    }
    return "unknown";
}

Error job_error(std::string code, std::string message, bool retryable) {
    return {std::move(code), std::move(message), {}, retryable};
}

} // namespace axk::app::job_runtime_detail

std::string_view axk::app::job_state_name(JobState state) noexcept {
    switch (state) {
    case JobState::queued:
        return "QUEUED";
    case JobState::running:
        return "RUNNING";
    case JobState::completed:
        return "COMPLETED";
    case JobState::failed:
        return "FAILED";
    case JobState::cancelled:
        return "CANCELLED";
    }
    return "FAILED";
}

bool axk::app::is_terminal(JobState state) noexcept {
    return state == JobState::completed || state == JobState::failed || state == JobState::cancelled;
}
