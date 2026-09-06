#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

#include "axklib/error.hpp"

namespace axk::detail::program_json_internal {

using Json = nlohmann::json;

inline Error invalid(std::string message) {
    return make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                      "Program parameters: " + std::move(message));
}

inline Result<void> fields(const Json &value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object())
        return std::unexpected{invalid("expected an object")};
    for (const auto &[name, ignored] : value.items()) {
        static_cast<void>(ignored);
        if (std::ranges::find(allowed, name) == allowed.end())
            return std::unexpected{invalid("unknown field '" + name + "'")};
    }
    return {};
}

template <typename T> Result<void> scalar(const Json &source, std::optional<T> &target) {
    if constexpr (std::is_same_v<T, bool>) {
        if (!source.is_boolean())
            return std::unexpected{invalid("expected a boolean")};
        target = source.get<bool>();
    } else {
        if (!source.is_number_integer())
            return std::unexpected{invalid("expected an integer")};
        if (source.is_number_unsigned()) {
            const auto parsed = source.get<std::uint64_t>();
            if (parsed > static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
                return std::unexpected{invalid("integer is outside its storage domain")};
            target = static_cast<T>(parsed);
        } else {
            const auto parsed = source.get<std::int64_t>();
            if (parsed < std::numeric_limits<T>::min() || parsed > std::numeric_limits<T>::max())
                return std::unexpected{invalid("integer is outside its storage domain")};
            target = static_cast<T>(parsed);
        }
    }
    return {};
}

template <typename T> Result<void> read(const Json &value, std::string_view name, std::optional<T> &target) {
    if (!value.contains(name))
        return {};
    return scalar(value[name], target);
}

template <typename T, typename Parser>
Result<void> child(const Json &value, std::string_view name, T &target, Parser parser) {
    if (!value.contains(name))
        return {};
    auto parsed = parser(value[name]);
    if (!parsed)
        return std::unexpected{parsed.error()};
    target = std::move(*parsed);
    return {};
}

template <typename T, std::size_t Size, typename Parser>
Result<void> numbered(const Json &value, std::array<T, Size> &target, Parser parser) {
    if (!value.is_object())
        return std::unexpected{invalid("numbered parameters must be an object")};
    for (const auto &[key, source] : value.items()) {
        std::size_t index{};
        const auto parsed = std::from_chars(key.data(), key.data() + key.size(), index);
        if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size() || index == 0U || index > Size ||
            key != std::to_string(index))
            return std::unexpected{invalid("invalid numbered parameter '" + key + "'")};
        if (auto result = parser(source, target[index - 1U]); !result)
            return result;
    }
    return {};
}

template <typename T, std::size_t Size, typename Parser>
Result<void> read_numbered(const Json &value, std::string_view name, std::array<T, Size> &target, Parser parser) {
    return value.contains(name) ? numbered(value[name], target, parser) : Result<void>{};
}

template <typename T> void write(Json &value, std::string_view name, const std::optional<T> &source) {
    if (source)
        value[name] = *source;
}

inline void write_group(Json &value, std::string_view name, Json group) {
    if (!group.empty())
        value[name] = std::move(group);
}

template <typename T, std::size_t Size, typename Writer>
Json write_numbered(const std::array<T, Size> &values, Writer writer) {
    auto result = Json::object();
    for (std::size_t index = 0; index < Size; ++index) {
        auto value = writer(values[index]);
        if (!value.is_null() && !value.empty())
            result[std::to_string(index + 1U)] = std::move(value);
    }
    return result;
}

template <typename T> Json scalar_json(const std::optional<T> &value) { return value ? Json(*value) : Json{}; }

} // namespace axk::detail::program_json_internal
