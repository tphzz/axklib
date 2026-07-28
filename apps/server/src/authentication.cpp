#include "authentication.hpp"

#include <algorithm>
#include <ranges>
#include <span>
#include <string_view>

#include "axklib/package_archive.hpp"

namespace axk::server::detail {
namespace {

bool constant_time_equal(std::string_view left, std::string_view right) {
    const auto size = std::max(left.size(), right.size());
    std::size_t difference = left.size() ^ right.size();
    for (std::size_t index = 0; index < size; ++index) {
        const auto left_byte = index < left.size() ? static_cast<unsigned char>(left[index]) : 0U;
        const auto right_byte = index < right.size() ? static_cast<unsigned char>(right[index]) : 0U;
        difference |= left_byte ^ right_byte;
    }
    return difference == 0U;
}

std::string token_digest(std::string_view token) {
    return package_internal::hex_digest(package_internal::sha256(std::as_bytes(std::span{token.data(), token.size()})));
}

} // namespace

std::optional<std::string> authenticated_principal(const Config &config, const crow::request &request) {
    constexpr std::string_view prefix{"Bearer "};
    const auto header = request.get_header_value("Authorization");
    if (!header.starts_with(prefix))
        return std::nullopt;
    const auto token = std::string_view{header}.substr(prefix.size());
    if (!config.bearer_token.empty() && constant_time_equal(token, config.bearer_token))
        return "loopback";
    const auto digest = token_digest(token);
    for (const auto &configured : config.token_hashes) {
        if (constant_time_equal(digest, configured.sha256))
            return configured.principal_id;
    }
    return std::nullopt;
}

bool origin_allowed(const Config &config, const crow::request &request) {
    const auto origin = request.get_header_value("Origin");
    return origin.empty() || std::ranges::find(config.allowed_origins, origin) != config.allowed_origins.end();
}

} // namespace axk::server::detail
