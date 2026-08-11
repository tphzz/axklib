#include "job_request_resources.hpp"

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace {

std::optional<std::string> destination_key(const nlohmann::json &request, std::string_view member) {
    const auto found = request.find(member);
    if (found == request.end() || !found->is_object())
        return std::nullopt;
    const auto root = found->find("rootId");
    const auto relative = found->find("relativePath");
    if (root == found->end() || relative == found->end() || !root->is_string() || !relative->is_string())
        return std::nullopt;

    std::string normalized;
    std::string_view remaining{relative->get_ref<const std::string &>()};
    while (!remaining.empty()) {
        const auto separator = remaining.find('/');
        const auto component = remaining.substr(0U, separator);
        if (!component.empty() && component != ".") {
            if (!normalized.empty())
                normalized.push_back('/');
            normalized.append(component);
        }
        if (separator == std::string_view::npos)
            break;
        remaining.remove_prefix(separator + 1U);
    }
    return root->get<std::string>() + '\0' + normalized;
}

} // namespace

std::vector<std::string> axk::app::job_detail::destination_keys(const nlohmann::json &request) {
    std::vector<std::string> result;
    for (const auto member : {"destination", "output"}) {
        if (auto key = destination_key(request, member); key && std::ranges::find(result, *key) == result.end())
            result.push_back(std::move(*key));
    }
    return result;
}

bool axk::app::job_detail::destinations_overlap(std::string_view left, std::string_view right) {
    const auto left_separator = left.find('\0');
    const auto right_separator = right.find('\0');
    if (left_separator == std::string_view::npos || right_separator == std::string_view::npos ||
        left.substr(0U, left_separator) != right.substr(0U, right_separator)) {
        return false;
    }
    left.remove_prefix(left_separator + 1U);
    right.remove_prefix(right_separator + 1U);
    if (left == right)
        return true;
    const auto is_parent = [](std::string_view parent, std::string_view child) {
        return !parent.empty() && child.size() > parent.size() && child.starts_with(parent) &&
               child[parent.size()] == '/';
    };
    return is_parent(left, right) || is_parent(right, left);
}

void axk::app::job_detail::collect_upload_ids(const nlohmann::json &value, std::vector<std::string> &result) {
    if (value.is_object()) {
        if (const auto reference = value.find("uploadRef"); reference != value.end() && reference->is_object()) {
            const auto upload_id = reference->find("uploadId");
            if (upload_id != reference->end() && upload_id->is_string()) {
                const auto &id = upload_id->get_ref<const std::string &>();
                if (std::ranges::find(result, id) == result.end())
                    result.push_back(id);
            }
        }
        for (const auto &item : value.items())
            collect_upload_ids(item.value(), result);
    } else if (value.is_array()) {
        for (const auto &item : value)
            collect_upload_ids(item, result);
    }
}
