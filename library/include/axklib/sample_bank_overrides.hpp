#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "axklib/object.hpp"
#include "axklib/sample_parameters.hpp"

namespace axk {

// An enable unit can govern several indexed fields or related selectors.
struct SampleBankOverrideUnit {
    std::uint8_t id{};
    std::vector<std::uint8_t> selectors;
    std::vector<std::string> keys;
};

struct SampleBankOverrideEdit {
    SampleParameters parameters;
    std::vector<std::uint8_t> enable;
    std::vector<std::uint8_t> disable;
};

AXK_API std::vector<SampleBankOverrideUnit> sample_bank_override_units(SampleParameterGeneration generation);
AXK_API bool sample_bank_override_state_supported(const CurrentSbac &bank);
// Changes only bank parameters and override flags. Members and membership are untouched.
AXK_API Result<void> apply_sample_bank_overrides(std::vector<std::byte> &payload, const SampleBankOverrideEdit &edit);

} // namespace axk
