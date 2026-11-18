#pragma once

#include "axklib/conversion/plan.hpp"

namespace axk::conversion {

[[nodiscard]] Registry builtin_profiles();
[[nodiscard]] nlohmann::json profiles_json(const Registry &registry = builtin_profiles());

} // namespace axk::conversion
