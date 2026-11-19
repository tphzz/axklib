#pragma once

#include "axklib/conversion/plan.hpp"

namespace axk::conversion::typhoon_to_a_series {
[[nodiscard]] Profile profile();
[[nodiscard]] std::vector<Profile> fresh_profiles();
} // namespace axk::conversion::typhoon_to_a_series
