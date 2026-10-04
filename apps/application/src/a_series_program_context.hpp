#pragma once

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include "axklib/application/image_sessions.hpp"
#include "axklib/catalog.hpp"
#include <nlohmann/json.hpp>

namespace axk::app::detail {
void add_program_editing_context(nlohmann::ordered_json &editing, std::string_view program_id,
                                 const std::unordered_map<std::string, ObjectSnapshot> &objects,
                                 std::span<const ImageRelationshipItem> relationships);
}
