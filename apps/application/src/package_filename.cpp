#include "package_filename.hpp"

#include <filesystem>
#include <format>

axk::app::Result<std::string> axk::app::package_internal::resolve_filename(std::string_view name,
                                                                           std::string_view required_extension) {
    if (name.empty() || name == "." || name == "..")
        return std::unexpected(Error{"invalid_request", "package output path is invalid"});
    auto result = std::string{name};
    const auto extension = std::filesystem::path{result}.extension().string();
    if (extension.empty()) {
        result += required_extension;
    } else if (extension != required_extension) {
        return std::unexpected(
            Error{"package_extension_mismatch", std::format("package output must use {}", required_extension)});
    }
    return result;
}
