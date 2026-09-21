#pragma once

#include <nlohmann/json.hpp>

#include "axklib/catalog.hpp"
#include "axklib/object.hpp"

namespace axk::app {
AXK_API nlohmann::json sample_format_metadata(const CurrentSbnk &sample);
AXK_API nlohmann::json sample_format_metadata(const CurrentSbac &bank);
AXK_API nlohmann::json sample_parameter_capabilities(const CurrentSbnk &sample);
AXK_API nlohmann::json sample_parameter_capabilities(const CurrentSbac &bank);
AXK_API nlohmann::json sample_format_conversion_previews(std::span<const std::byte> payload, bool bank = false);
AXK_API nlohmann::json object_format_conversion(const ObjectSnapshot &snapshot, std::span<const std::byte> payload,
                                                bool writable);
} // namespace axk::app
