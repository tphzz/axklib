#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "axklib/program_storage.hpp"

namespace axk {

struct ProgramFormatConversionIssue {
    std::string key;
    std::string message;
    std::optional<std::int64_t> stored_value;
};

struct ProgramFormatConversionPlan {
    ProgramStorageInfo source;
    ProgramStorageFormat target{ProgramStorageFormat::unknown};
    bool no_op{};
    std::vector<std::string> changes;
    std::vector<ProgramFormatConversionIssue> blockers;
    // Inspection and execution share this planner; blocked plans contain no replacement.
    std::vector<std::byte> converted_payload;

    [[nodiscard]] bool allowed() const { return blockers.empty() && !converted_payload.empty(); }
};

AXK_API ProgramFormatConversionPlan plan_program_format_conversion(std::span<const std::byte> payload,
                                                                   ProgramStorageFormat target);

} // namespace axk
