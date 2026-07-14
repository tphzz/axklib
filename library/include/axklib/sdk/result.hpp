#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace axk {

enum class error_category : std::uint8_t {
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

enum class error_code : std::uint32_t {
    invalid_argument = 1,
    out_of_bounds = 2,
    integer_overflow = 3,
    invalid_ascii = 4,
    io_open_failed = 100,
    io_read_failed = 101,
    io_short_read = 102,
    io_unsupported_size = 103,
    container_unrecognized = 200,
    container_truncated = 201,
    container_invalid_geometry = 202,
    container_backup_mismatch = 203,
    container_partition_out_of_range = 204,
    allocation_invalid_extent = 300,
    allocation_cycle = 301,
    allocation_mismatch = 302,
    object_malformed = 400,
    object_missing = 401,
    relationship_unresolved = 500,
    relationship_ambiguous = 501,
    relationship_cycle = 502,
    audio_unsupported_format = 600,
    manifest_invalid = 700,
    transaction_rejected = 750,
    unsupported_profile = 800,
    operation_cancelled = 900,
    internal_invariant = 1000,
};

struct error_context {
    std::optional<std::string> source_path;
    std::optional<std::uint8_t> partition_index;
    std::optional<std::string> volume_name;
    std::optional<std::string> object_type;
    std::optional<std::string> object_name;
    std::optional<std::uint64_t> raw_offset;
};

struct error {
    error_code code{error_code::invalid_argument};
    error_category category{error_category::internal};
    std::string message;
    error_context context;
};

class bad_result_access final : public std::logic_error {
  public:
    bad_result_access()
        : std::logic_error("axk::result holds the other alternative") {}
};

template <typename T> class result {
  public:
    result(const T &value) : storage_(value) {}
    result(T &&value) : storage_(std::move(value)) {}
    result(const axk::error &failure) : storage_(failure) {}
    result(axk::error &&failure) : storage_(std::move(failure)) {}

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(storage_);
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    T &value() & {
        if (!has_value())
            throw bad_result_access{};
        return std::get<T>(storage_);
    }
    const T &value() const & {
        if (!has_value())
            throw bad_result_access{};
        return std::get<T>(storage_);
    }
    T &&value() && {
        if (!has_value())
            throw bad_result_access{};
        return std::get<T>(std::move(storage_));
    }
    axk::error &error() & {
        if (has_value())
            throw bad_result_access{};
        return std::get<axk::error>(storage_);
    }
    const axk::error &error() const & {
        if (has_value())
            throw bad_result_access{};
        return std::get<axk::error>(storage_);
    }
    T &operator*() & { return value(); }
    const T &operator*() const & { return value(); }
    T *operator->() { return &value(); }
    const T *operator->() const { return &value(); }

  private:
    std::variant<T, axk::error> storage_;
};

template <> class result<void> {
  public:
    result() noexcept = default;
    result(const axk::error &failure) : failure_(failure) {}
    result(axk::error &&failure) : failure_(std::move(failure)) {}

    [[nodiscard]] bool has_value() const noexcept {
        return !failure_.has_value();
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }
    void value() const {
        if (!has_value())
            throw bad_result_access{};
    }
    axk::error &error() & {
        if (has_value())
            throw bad_result_access{};
        return *failure_;
    }
    const axk::error &error() const & {
        if (has_value())
            throw bad_result_access{};
        return *failure_;
    }

  private:
    std::optional<axk::error> failure_;
};

} // namespace axk
