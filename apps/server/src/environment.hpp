#pragma once

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace axk::server::detail {

inline std::optional<std::string> environment_variable(std::string_view name) {
    const std::string key{name};
#if defined(_MSC_VER)
    char *raw{};
    std::size_t size{};
    if (_dupenv_s(&raw, &size, key.c_str()) != 0 || raw == nullptr)
        return std::nullopt;
    const std::unique_ptr<char, decltype(&std::free)> value{raw, &std::free};
    return std::string{value.get()};
#else
    const auto *value = std::getenv(key.c_str());
    return value == nullptr ? std::nullopt : std::optional<std::string>{value};
#endif
}

} // namespace axk::server::detail
