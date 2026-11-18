#include "axklib/sdk/source_set.hpp"

#include <type_traits>

static_assert(std::is_nothrow_move_constructible_v<axk::source_set>);
static_assert(!std::is_copy_constructible_v<axk::source_set>);
