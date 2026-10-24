#include "read_diagnostics.hpp"

#include <string>
#include <utility>

namespace axk::app {
namespace {
struct MetricDescription {
    std::string_view name;
    bool nanoseconds{};
};
constexpr std::array<MetricDescription, static_cast<std::size_t>(ReadMetric::count)> descriptions{
    {{"transportReads"},
     {"transportReadBytes"},
     {"transportReadTimeUs", true},
     {"failedReads"},
     {"readLe4KiB"},
     {"readLe64KiB"},
     {"readLe1MiB"},
     {"readGt1MiB"},
     {"cacheHits"},
     {"cacheMisses"},
     {"bindingChecks"},
     {"bindingTimeUs", true},
     {"enumerationTimeUs", true},
     {"layoutGuardReads"},
     {"layoutGuardBytes"},
     {"layoutGuardTimeUs", true},
     {"pcmReadCalls"},
     {"pcmReadBytes"},
     {"pcmReadTimeUs", true},
     {"binningTimeUs", true},
     {"connectionLockWaitUs", true},
     {"transportLockWaitUs", true},
     {"requestedBins"},
     {"storedFrames"},
     {"lanes"},
     {"helperReads"},
     {"directReads"},
     {"identityTimeUs", true},
     {"sessionLockWaitUs", true}}};
std::uint64_t elapsed_us(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}
} // namespace

ReadDiagnosticScope::ReadDiagnosticScope(diagnostic_sink *sink, std::string_view event,
                                         const CancellationToken *cancellation) noexcept
    : event_(event), previous_(current_), cancellation_(cancellation) {
    if (sink && sink->enabled(diagnostic_level::debug)) {
        sink_ = sink;
        started_ = Clock::now();
        phases_[0] = {.name = "resolve", .started = started_};
        phase_count_ = 1U;
        current_ = this;
    } else {
        current_ = nullptr;
    }
}

ReadDiagnosticScope::~ReadDiagnosticScope() {
    current_ = previous_;
    if (!sink_)
        return;
    const auto ended = Clock::now();
    close_phase(ended);
    for (std::size_t index = 0U; index < phase_count_; ++index) {
        const auto &item = phases_[index];
        emit(item.name, item.duration_us, item.counters, elapsed_us(started_, item.started));
    }
    emit("total", elapsed_us(started_, ended), total_);
}

void ReadDiagnosticScope::close_phase(Clock::time_point now) noexcept {
    if (phase_count_)
        phases_[phase_count_ - 1U].duration_us = elapsed_us(phases_[phase_count_ - 1U].started, now);
}

void ReadDiagnosticScope::phase(std::string_view name) noexcept {
    if (!sink_ || phase_count_ == phases_.size())
        return;
    const auto now = Clock::now();
    close_phase(now);
    phases_[phase_count_++] = {.name = name, .started = now};
}

void ReadDiagnosticScope::add(ReadMetric metric, std::uint64_t value) noexcept {
    if (!current_)
        return;
    const auto index = static_cast<std::size_t>(metric);
    current_->total_[index] += value;
    current_->phases_[current_->phase_count_ - 1U].counters[index] += value;
}

void ReadDiagnosticScope::record_read(std::size_t bytes, std::uint64_t nanoseconds, bool success) noexcept {
    add(ReadMetric::transport_reads, 1U);
    add(ReadMetric::transport_bytes, bytes);
    add(ReadMetric::transport_time, nanoseconds);
    add(ReadMetric::failed_reads, success ? 0U : 1U);
    add(bytes <= 4096U      ? ReadMetric::read_le_4k
        : bytes <= 65536U   ? ReadMetric::read_le_64k
        : bytes <= 1048576U ? ReadMetric::read_le_1m
                            : ReadMetric::read_gt_1m,
        1U);
}

void ReadDiagnosticScope::emit(std::string_view phase_name, std::uint64_t duration_us, const Counters &counters,
                               std::uint64_t start_offset_us) noexcept {
    try {
        diagnostic_event event{.level = diagnostic_level::debug,
                               .source = diagnostic_source::devices,
                               .event = std::string{event_},
                               .phase = std::string{phase_name},
                               .outcome = completed_                                       ? "success"
                                          : cancellation_ && cancellation_->is_cancelled() ? "cancelled"
                                                                                           : "failed",
                               .duration_us = duration_us,
                               .metrics = {}};
        event.metrics.reserve(counters.size() + 1U);
        event.metrics.push_back({"startOffsetUs", start_offset_us});
        for (std::size_t index = 0U; index < counters.size(); ++index)
            if (counters[index])
                event.metrics.push_back({std::string{descriptions[index].name},
                                         counters[index] / (descriptions[index].nanoseconds ? 1000U : 1U)});
        sink_->report(event);
    } catch (...) {
    }
}
} // namespace axk::app
