#pragma once

#include "axklib/sdk/diagnostics.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace axk::app {

enum class ReadMetric : std::size_t {
    transport_reads,
    transport_bytes,
    transport_time,
    failed_reads,
    read_le_4k,
    read_le_64k,
    read_le_1m,
    read_gt_1m,
    cache_hits,
    cache_misses,
    binding_checks,
    binding_time,
    enumeration_time,
    layout_reads,
    layout_bytes,
    layout_time,
    pcm_calls,
    pcm_bytes,
    pcm_time,
    binning_time,
    connection_wait,
    transport_wait,
    requested_bins,
    stored_frames,
    lanes,
    helper_reads,
    direct_reads,
    count
};

// Synchronous request-local collection, including nested device calls. No shared
// cumulative snapshots: another session on the same carrier must not be attributed here.
class ReadDiagnosticScope {
  public:
    ReadDiagnosticScope(diagnostic_sink *sink, std::string_view event) noexcept;
    ~ReadDiagnosticScope();
    ReadDiagnosticScope(const ReadDiagnosticScope &) = delete;
    ReadDiagnosticScope &operator=(const ReadDiagnosticScope &) = delete;
    void phase(std::string_view name) noexcept;
    void complete() noexcept { completed_ = true; }
    static bool active() noexcept { return current_ != nullptr; }
    static void add(ReadMetric metric, std::uint64_t value) noexcept;
    static void record_read(std::size_t bytes, std::uint64_t nanoseconds, bool success) noexcept;

  private:
    using Clock = std::chrono::steady_clock;
    using Counters = std::array<std::uint64_t, static_cast<std::size_t>(ReadMetric::count)>;
    struct Phase {
        std::string_view name;
        Clock::time_point started{};
        std::uint64_t duration_us{};
        Counters counters{};
    };
    diagnostic_sink *sink_{};
    std::string_view event_;
    ReadDiagnosticScope *previous_{};
    Clock::time_point started_{};
    std::array<Phase, 12U> phases_{};
    std::size_t phase_count_{};
    Counters total_{};
    bool completed_{};
    inline static thread_local ReadDiagnosticScope *current_{};
    void close_phase(Clock::time_point now) noexcept;
    void emit(std::string_view phase, std::uint64_t duration_us, const Counters &counters,
              std::uint64_t start_offset_us = 0U) noexcept;
};

class ReadDiagnosticTimer {
  public:
    explicit ReadDiagnosticTimer(ReadMetric metric) noexcept
        : metric_(metric), enabled_(ReadDiagnosticScope::active()),
          started_(enabled_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
    ~ReadDiagnosticTimer() {
        if (enabled_)
            ReadDiagnosticScope::add(metric_,
                                     static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                                    std::chrono::steady_clock::now() - started_)
                                                                    .count()));
    }
    ReadDiagnosticTimer(const ReadDiagnosticTimer &) = delete;
    ReadDiagnosticTimer &operator=(const ReadDiagnosticTimer &) = delete;

  private:
    ReadMetric metric_;
    bool enabled_;
    std::chrono::steady_clock::time_point started_;
};
} // namespace axk::app
