#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>

#include "axklib/export.hpp"

namespace axk {

enum class ErrorCategory : std::uint8_t {
    io,
    container,
    allocation,
    object,
    relationship,
    audio,
    manifest,
    transaction,
    unsupported,
    cancelled,
    internal,
};

enum class ErrorCode : std::uint32_t {
    // Core and checked-access errors: 1-99.
    invalid_argument = 1,
    out_of_bounds = 2,
    integer_overflow = 3,
    invalid_ascii = 4,
    // I/O errors: 100-199.
    io_open_failed = 100,
    io_read_failed = 101,
    io_short_read = 102,
    io_unsupported_size = 103,
    // Container errors: 200-299.
    container_unrecognized = 200,
    container_truncated = 201,
    container_invalid_geometry = 202,
    container_backup_mismatch = 203,
    container_partition_out_of_range = 204,
    // Allocation errors: 300-399.
    allocation_invalid_extent = 300,
    allocation_cycle = 301,
    allocation_mismatch = 302,
    allocation_cross_link = 303,
    // Object errors: 400-499.
    object_malformed = 400,
    object_missing = 401,
    // Relationship errors: 500-599.
    relationship_unresolved = 500,
    relationship_ambiguous = 501,
    relationship_cycle = 502,
    // Audio errors: 600-699.
    audio_unsupported_format = 600,
    audio_wave_data_too_large = 601,
    // Manifest and transaction errors: 700-799.
    manifest_invalid = 700,
    transaction_rejected = 750,
    // Unsupported writer/profile errors: 800-899.
    unsupported_profile = 800,
    operation_cancelled = 900,
    internal_invariant = 1000,
};

struct ErrorContext {
    std::optional<std::string> source_path;
    std::optional<std::uint8_t> partition_index;
    std::optional<std::string> volume_name;
    std::optional<std::string> object_type;
    std::optional<std::string> object_name;
    std::optional<std::uint64_t> raw_offset;

    friend bool operator==(const ErrorContext &, const ErrorContext &) = default;
};

struct Error {
    ErrorCode code{ErrorCode::invalid_argument};
    ErrorCategory category{ErrorCategory::internal};
    std::string message;
    ErrorContext context;

    friend bool operator==(const Error &, const Error &) = default;
};

template <typename T> using Result = std::expected<T, Error>;

inline Error make_error(ErrorCode code, ErrorCategory category, std::string message, ErrorContext context = {}) {
    return Error{code, category, std::move(message), std::move(context)};
}

AXK_API std::string render_error(const Error &error, bool include_trace = true);

} // namespace axk
