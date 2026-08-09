#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "axklib/error.hpp"

namespace axk {

Error sequence_channel_size_error(std::uint8_t status, std::size_t expected, std::size_t observed);
Error sequence_event_error(Error error, std::string_view name, std::size_t offset, std::size_t block_index,
                           std::size_t event_index, std::uint32_t tick);

} // namespace axk
