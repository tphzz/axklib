#pragma once

#include <optional>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

#include "axklib/application/extraction_selection.hpp"

namespace axk::app {

[[nodiscard]] nlohmann::json relationship_diagnostic(const ExcludedExtractionRelationship &relationship,
                                                     std::string_view message,
                                                     std::optional<std::string_view> source = std::nullopt,
                                                     std::optional<std::string_view> selector = std::nullopt);

} // namespace axk::app
