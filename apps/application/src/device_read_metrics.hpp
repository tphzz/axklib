#pragma once

#include <chrono>
#include <cstdint>

#include <nlohmann/json.hpp>

#include "axklib/io.hpp"

namespace axk::app::device_detail {

struct ReadCounters {
    std::uint64_t calls{};
    std::uint64_t bytes{};
    std::uint64_t nanoseconds{};
    std::uint64_t binding_checks{}, binding_nanoseconds{};
    std::uint64_t writes{}, written_bytes{}, write_nanoseconds{};
    std::uint64_t flushes{}, flush_nanoseconds{};
    std::uint64_t journal_publications{}, journal_publication_nanoseconds{};
    std::uint64_t state_publications{}, state_publication_nanoseconds{};
    std::uint64_t verification_passes{};
    std::uint64_t verification_reads{};
    std::uint64_t verified_bytes{};
    std::uint64_t verification_gap_bytes{};
};

nlohmann::json device_read_metrics(const RandomAccessReader *reader);
void record_device_journal_publication(const RandomAccessReader *reader, std::chrono::steady_clock::duration elapsed);

} // namespace axk::app::device_detail
