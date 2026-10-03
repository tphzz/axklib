#pragma once

#include <cstddef>
#include <span>

#include <nlohmann/json.hpp>

#include "axklib/catalog.hpp"
#include "axklib/object.hpp"

namespace axk::app {
AXK_API nlohmann::json program_format_metadata(const DecodedObject &object);
AXK_API nlohmann::json program_format_conversion(const ObjectSnapshot &snapshot, std::span<const std::byte> payload,
                                                 bool writable);
} // namespace axk::app
