#pragma once

#include <nlohmann/json.hpp>

#include "axklib/object.hpp"

namespace axk::app {
AXK_API nlohmann::json sample_format_metadata(const CurrentSbnk &sample);
AXK_API nlohmann::json sample_parameter_capabilities(const CurrentSbnk &sample);
AXK_API nlohmann::json sample_format_conversion_previews(std::span<const std::byte> payload);
} // namespace axk::app
