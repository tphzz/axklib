#pragma once

#include <cstdint>
#include <string_view>

#include "axklib/generated/current_lookups.hpp"

namespace axk {

using CurrentLookup = generated::LookupId;

constexpr std::string_view current_label(CurrentLookup table,
                                         std::int32_t key) noexcept {
    return generated::lookup(table, key);
}

} // namespace axk
