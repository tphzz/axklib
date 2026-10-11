#include "axklib/sdk/devices.hpp"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<axk::device_host>);
static_assert(std::is_nothrow_move_constructible_v<axk::device_host>);
