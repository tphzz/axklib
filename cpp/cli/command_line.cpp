#include "command_line.hpp"

#ifdef _WIN32

#include "axklib/utf8.hpp"

namespace axk::cli::platform {

Result<std::vector<std::string>> normalize_windows_command_line(
    std::span<wchar_t* const> arguments) {
  static_assert(sizeof(wchar_t) == sizeof(char16_t));
  std::vector<std::string> result;
  result.reserve(arguments.size());
  for (const auto* argument : arguments) {
    if (argument == nullptr) {
      return std::unexpected{make_error(ErrorCode::invalid_argument, ErrorCategory::internal,
                                        "command-line argument is null")};
    }
    std::u16string utf16;
    for (const auto* cursor = argument; *cursor != L'\0'; ++cursor)
      utf16.push_back(static_cast<char16_t>(*cursor));
    auto converted = text::utf16_to_utf8(utf16);
    if (!converted)
      return std::unexpected{converted.error()};
    result.push_back(std::move(*converted));
  }
  return result;
}

}  // namespace axk::cli::platform

#endif
