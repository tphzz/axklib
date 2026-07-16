#include "axklib/server/request_validation.hpp"

#include <cstddef>
#include <ranges>

namespace axk::server {
namespace {

bool consume_json_budget(const nlohmann::json &value, const Config &config, std::size_t depth, std::size_t &nodes) {
    if (depth > config.maximum_json_depth || ++nodes > config.maximum_json_nodes)
        return false;
    if (value.is_string())
        return value.get_ref<const std::string &>().size() <= config.maximum_json_string_bytes;
    if (value.is_array()) {
        if (value.size() > config.maximum_json_container_items)
            return false;
        return std::ranges::all_of(
            value, [&](const auto &item) { return consume_json_budget(item, config, depth + 1U, nodes); });
    }
    if (value.is_object()) {
        if (value.size() > config.maximum_json_container_items)
            return false;
        return std::ranges::all_of(value.items(), [&](const auto &item) {
            return item.key().size() <= config.maximum_json_string_bytes &&
                   consume_json_budget(item.value(), config, depth + 1U, nodes);
        });
    }
    return true;
}

} // namespace

app::Result<nlohmann::json> parse_json_request(std::string_view body, const Config &config) {
    if (body.size() > config.maximum_json_bytes)
        return std::unexpected(app::Error{"request_too_large", "JSON request exceeds the configured byte limit"});
    auto input = nlohmann::json::parse(body, nullptr, false);
    if (input.is_discarded() || !input.is_object())
        return std::unexpected(app::Error{"invalid_json", "request body must be one JSON object"});
    std::size_t nodes{};
    if (!consume_json_budget(input, config, 1U, nodes)) {
        return std::unexpected(
            app::Error{"json_structure_too_large", "JSON request exceeds the configured structure limits"});
    }
    return input;
}

} // namespace axk::server
