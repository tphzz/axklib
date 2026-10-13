#pragma once

#include "axklib/sdk/diagnostics.hpp"
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace axk {
class DiagnosticTimer {
  public:
    DiagnosticTimer(diagnostic_sink *sink, diagnostic_source source, std::string_view event, std::string_view phase,
                    diagnostic_level level = diagnostic_level::debug)
        : sink_(sink), source_(source), event_(event), phase_(phase), level_(level) {
        if (sink_ != nullptr && sink_->enabled(level_))
            started_ = std::chrono::steady_clock::now();
    }
    ~DiagnosticTimer() { finish("incomplete"); }
    DiagnosticTimer(const DiagnosticTimer &) = delete;
    DiagnosticTimer &operator=(const DiagnosticTimer &) = delete;
    void finish(std::string_view outcome = "success",
                std::initializer_list<std::pair<std::string_view, std::uint64_t>> metrics = {}) noexcept {
        if (finished_)
            return;
        finished_ = true;
        if (sink_ == nullptr || started_ == std::chrono::steady_clock::time_point{} || !sink_->enabled(level_))
            return;
        try {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started_)
                    .count();
            diagnostic_event value{level_,
                                   source_,
                                   std::string{event_},
                                   std::string{phase_},
                                   std::string{outcome},
                                   static_cast<std::uint64_t>(elapsed),
                                   {}};
            value.metrics.reserve(metrics.size());
            for (const auto &[name, count] : metrics)
                value.metrics.push_back({std::string{name}, count});
            sink_->report(value);
        } catch (...) {
        }
    }

  private:
    diagnostic_sink *sink_{};
    diagnostic_source source_;
    std::string_view event_;
    std::string_view phase_;
    diagnostic_level level_;
    std::chrono::steady_clock::time_point started_{};
    bool finished_{};
};
} // namespace axk
