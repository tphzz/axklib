#include "axklib/sdk/conversion.hpp"

#include <type_traits>

static_assert(std::is_move_constructible_v<axk::conversion_plan>);
static_assert(!std::is_copy_constructible_v<axk::conversion_plan>);
