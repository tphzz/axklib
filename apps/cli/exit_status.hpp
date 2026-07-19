#pragma once

#include <string_view>

#include "axklib/error.hpp"

namespace axk::cli {

enum class ExitStatus : int {
    success = 0,
    operational_failure = 1,
    invalid_request = 2,
    diagnostics = 3,
};

[[nodiscard]] constexpr int exit_code(ExitStatus status) noexcept { return static_cast<int>(status); }

[[nodiscard]] constexpr ExitStatus application_error_status(std::string_view code) noexcept {
    if (code.starts_with("invalid_") || code.starts_with("unsupported_") || code == "unknown_operation" ||
        code == "selector_not_found" || code == "root_not_found" || code == "entry_not_found" ||
        code == "path_outside_sandbox" || code == "manifest_kind_mismatch" || code == "manifest_size" ||
        code == "package_extension_mismatch" || code == "upload_kind_mismatch" || code == "write_plan_kind_mismatch" ||
        code == "write_plan_not_found" || code == "package_plan_not_found") {
        return ExitStatus::invalid_request;
    }
    return ExitStatus::operational_failure;
}

[[nodiscard]] constexpr ExitStatus core_error_status(const axk::Error &error) noexcept {
    if (error.code == axk::ErrorCode::invalid_argument)
        return ExitStatus::invalid_request;
    switch (error.category) {
    case axk::ErrorCategory::io:
    case axk::ErrorCategory::cancelled:
    case axk::ErrorCategory::internal:
        return ExitStatus::operational_failure;
    case axk::ErrorCategory::container:
    case axk::ErrorCategory::allocation:
    case axk::ErrorCategory::object:
    case axk::ErrorCategory::relationship:
    case axk::ErrorCategory::audio:
    case axk::ErrorCategory::manifest:
    case axk::ErrorCategory::transaction:
    case axk::ErrorCategory::unsupported:
        return ExitStatus::invalid_request;
    }
    return ExitStatus::operational_failure;
}

} // namespace axk::cli
