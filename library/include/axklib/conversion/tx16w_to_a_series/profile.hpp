#pragma once

#include "axklib/conversion/plan.hpp"

namespace axk::tx16w::a_series {
[[nodiscard]] conversion::Profile profile();
[[nodiscard]] std::vector<conversion::Profile> fresh_profiles();
} // namespace axk::tx16w::a_series
