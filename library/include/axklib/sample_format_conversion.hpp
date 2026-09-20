#pragma once

#include <span>
#include <string>
#include <vector>

#include "axklib/sample_parameter_rules.hpp"
#include "axklib/sample_storage.hpp"

namespace axk {

struct SampleFormatConversionPlan {
    SampleStorageInfo source;
    SampleStorageFormat target{SampleStorageFormat::unknown};
    bool no_op{};
    std::vector<std::string> changes;
    std::vector<SampleParameterIssue> blockers;
    // Populated only for an allowed plan. Inspection and execution use the same planner.
    std::vector<std::byte> converted_payload;

    [[nodiscard]] bool allowed() const { return blockers.empty() && !converted_payload.empty(); }
};

AXK_API SampleFormatConversionPlan plan_sample_format_conversion(std::span<const std::byte> payload,
                                                                 SampleStorageFormat target);

} // namespace axk
