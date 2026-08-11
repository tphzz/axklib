#include "sdk_internal.hpp"

#include "axklib/sdk/version.hpp"

#include <string_view>
#include <utility>

#include "axklib/utf8.hpp"
#include "axklib/version.hpp"

namespace axk {
namespace sdk_internal {
namespace {
error_code public_error_code(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::invalid_argument:
        return error_code::invalid_argument;
    case ErrorCode::out_of_bounds:
        return error_code::out_of_bounds;
    case ErrorCode::integer_overflow:
        return error_code::integer_overflow;
    case ErrorCode::invalid_ascii:
        return error_code::invalid_ascii;
    case ErrorCode::io_open_failed:
        return error_code::io_open_failed;
    case ErrorCode::io_read_failed:
        return error_code::io_read_failed;
    case ErrorCode::io_short_read:
        return error_code::io_short_read;
    case ErrorCode::io_unsupported_size:
        return error_code::io_unsupported_size;
    case ErrorCode::container_unrecognized:
        return error_code::container_unrecognized;
    case ErrorCode::container_truncated:
        return error_code::container_truncated;
    case ErrorCode::container_invalid_geometry:
        return error_code::container_invalid_geometry;
    case ErrorCode::container_backup_mismatch:
        return error_code::container_backup_mismatch;
    case ErrorCode::container_partition_out_of_range:
        return error_code::container_partition_out_of_range;
    case ErrorCode::allocation_invalid_extent:
        return error_code::allocation_invalid_extent;
    case ErrorCode::allocation_cycle:
        return error_code::allocation_cycle;
    case ErrorCode::allocation_mismatch:
        return error_code::allocation_mismatch;
    case ErrorCode::allocation_cross_link:
        return error_code::allocation_cross_link;
    case ErrorCode::object_malformed:
        return error_code::object_malformed;
    case ErrorCode::object_missing:
        return error_code::object_missing;
    case ErrorCode::relationship_unresolved:
        return error_code::relationship_unresolved;
    case ErrorCode::relationship_ambiguous:
        return error_code::relationship_ambiguous;
    case ErrorCode::relationship_cycle:
        return error_code::relationship_cycle;
    case ErrorCode::audio_unsupported_format:
        return error_code::audio_unsupported_format;
    case ErrorCode::audio_wave_data_too_large:
        return error_code::audio_wave_data_too_large;
    case ErrorCode::manifest_invalid:
        return error_code::manifest_invalid;
    case ErrorCode::transaction_rejected:
        return error_code::transaction_rejected;
    case ErrorCode::transaction_stale:
        return error_code::transaction_stale;
    case ErrorCode::unsupported_profile:
        return error_code::unsupported_profile;
    case ErrorCode::operation_cancelled:
        return error_code::operation_cancelled;
    case ErrorCode::internal_invariant:
        return error_code::internal_invariant;
    }
    return error_code::internal_invariant;
}

error_category public_error_category(ErrorCategory category) noexcept {
    switch (category) {
    case ErrorCategory::io:
        return error_category::io;
    case ErrorCategory::container:
        return error_category::container;
    case ErrorCategory::allocation:
        return error_category::allocation;
    case ErrorCategory::object:
        return error_category::object;
    case ErrorCategory::relationship:
        return error_category::relationship;
    case ErrorCategory::audio:
        return error_category::audio;
    case ErrorCategory::manifest:
        return error_category::manifest;
    case ErrorCategory::transaction:
        return error_category::transaction;
    case ErrorCategory::unsupported:
        return error_category::unsupported;
    case ErrorCategory::cancelled:
        return error_category::cancelled;
    case ErrorCategory::internal:
        return error_category::internal;
    }
    return error_category::internal;
}

} // namespace

error public_error(const Error &failure) {
    error_context context;
    context.source_path = failure.context.source_path;
    context.partition_index = failure.context.partition_index;
    context.volume_name = failure.context.volume_name;
    context.object_type = failure.context.object_type;
    context.object_name = failure.context.object_name;
    context.raw_offset = failure.context.raw_offset;
    return {
        public_error_code(failure.code),
        public_error_category(failure.category),
        failure.message,
        std::move(context),
    };
}

error invalid_argument(std::string message) {
    return {error_code::invalid_argument, error_category::internal, std::move(message), {}};
}

error internal_error(std::string message) {
    return {error_code::internal_invariant, error_category::internal, std::move(message), {}};
}

result<std::filesystem::path> checked_path(const std::string &value, std::string label) {
    if (value.empty())
        return invalid_argument(std::move(label) + " is required");
    auto path = text::path_from_utf8(value);
    if (!path)
        return public_error(path.error());
    return std::move(*path);
}

} // namespace sdk_internal

void operation_context::impl::report(const Progress &progress) noexcept {
    const std::scoped_lock lock{mutex};
    if (destination == nullptr)
        return;
    progress_event event{
        static_cast<std::uint32_t>(progress.phase),
        progress.completed,
        progress.total,
        progress.label,
        progress.output_path,
    };
    try {
        destination->report(event);
    } catch (...) {
        // A progress observer cannot abort or unwind a native operation.
    }
}

progress_sink::~progress_sink() = default;

operation_context::operation_context() : impl_(std::make_unique<impl>()) {}
operation_context::~operation_context() = default;
operation_context::operation_context(operation_context &&) noexcept = default;
operation_context &operation_context::operator=(operation_context &&) noexcept = default;
void operation_context::cancel() noexcept {
    if (impl_)
        impl_->cancellation.cancel();
}
void operation_context::reset_cancel() noexcept {
    if (impl_)
        impl_->cancellation.reset();
}
void operation_context::set_progress_sink(progress_sink *sink) noexcept {
    if (!impl_)
        return;
    const std::scoped_lock lock{impl_->mutex};
    impl_->destination = sink;
}

std::string sdk_version() { return version_string; }

build_info sdk_build_info() noexcept { return current_build_info(); }

std::string render_error(const error &failure) {
    ErrorContext context;
    context.source_path = failure.context.source_path;
    context.partition_index = failure.context.partition_index;
    context.volume_name = failure.context.volume_name;
    context.object_type = failure.context.object_type;
    context.object_name = failure.context.object_name;
    context.raw_offset = failure.context.raw_offset;
    return axk::render_error(Error{static_cast<ErrorCode>(failure.code), static_cast<ErrorCategory>(failure.category),
                                   failure.message, std::move(context)});
}

} // namespace axk
