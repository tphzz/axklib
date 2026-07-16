#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "axklib/error.hpp"

namespace axk::text {

bool is_valid_utf8(std::string_view value);
Result<std::string> utf16_to_utf8(std::u16string_view value);
Result<std::filesystem::path> path_from_utf8(std::string_view value);
std::string path_to_utf8(const std::filesystem::path &value);
Result<std::filesystem::path> temporary_sibling(const std::filesystem::path &target, std::string_view suffix = {});

} // namespace axk::text
