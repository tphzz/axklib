#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "axklib/application/jobs.hpp"

namespace axk::app::job_runtime_detail {

[[nodiscard]] std::uint64_t timestamp_unix_ms();
[[nodiscard]] std::uint64_t elapsed_ms(std::chrono::steady_clock::time_point start,
                                       std::chrono::steady_clock::time_point end) noexcept;
[[nodiscard]] std::string progress_phase_name(ProgressPhase phase);
[[nodiscard]] Error job_error(std::string code, std::string message, bool retryable = false);

} // namespace axk::app::job_runtime_detail
