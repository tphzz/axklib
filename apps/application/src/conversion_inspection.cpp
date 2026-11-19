#include "conversion_inspection.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace axk::app::conversion_internal {
Result<nlohmann::json> inspect_conversion_source(const nlohmann::json &input, const conversion::Registry &profiles,
                                                 const native::FormatRegistry &formats, const Sandbox &sandbox,
                                                 UploadStore &uploads, ImageSessionManager &images,
                                                 const OperationContext &context) {
    using Json = nlohmann::json;
    try {
        if (!input.is_object() || !input.contains("source"))
            return std::unexpected(Error{"invalid_request", "Select a conversion source"});
        for (const auto &key : input.items())
            if (key.key() != "source" && key.key() != "sourceFormat" && key.key() != "objectType" &&
                key.key() != "offset" && key.key() != "limit")
                return std::unexpected(Error{"invalid_request", "Unknown conversion inspection field"});
        for (const auto key : {"offset", "limit"})
            if (input.contains(key) && (!input[key].is_number_integer() || input[key] < 0))
                return std::unexpected(
                    Error{"invalid_request", "Inspection pagination requires non-negative integers"});
        const auto offset = input.value("offset", std::uint64_t{0});
        const auto limit = input.value("limit", std::uint64_t{100});
        if (limit == 0 || limit > 200)
            return std::unexpected(Error{"invalid_request", "Inspection page limit must be 1..200"});
        const auto format = input.value("sourceFormat", std::string{});
        if (input.at("source").at("kind") == "SOURCES" && format.empty())
            return std::unexpected(Error{"invalid_request", "An explicit disk set requires its source format"});
        auto source = open_source(input.at("source"), formats, format, sandbox, uploads, images, context,
                                  conversion::Limits{}.maximum_bytes);
        if (!source)
            return std::unexpected(source.error());
        const auto &summary = source->document->summary();
        if (input.contains("sourceFormat") && summary.format_id != format)
            return std::unexpected(Error{"invalid_request", "Source document does not match the requested format"});
        auto applicable = Json::array();
        for (const auto &profile : profiles.profiles())
            if (profile.source_format == summary.format_id)
                applicable.push_back(profile.id);
        const auto type = input.value("objectType", std::string{});
        std::size_t total{};
        auto rows = Json::array();
        // Translate only the returned page, not the entire server object index.
        std::unordered_map<std::string_view, std::size_t> page_ids;
        for (const auto &object : source->document->catalog().objects) {
            if (!context.cancellation.check())
                return std::unexpected(Error{"operation_cancelled", "Conversion inspection cancelled"});
            if (!type.empty() && object.type != type)
                continue;
            if (total++ < offset || rows.size() == limit)
                continue;
            page_ids.emplace(object.id, rows.size());
            rows.push_back({{"id", object.id},
                            {"name", object.name},
                            {"type", object.type},
                            {"volumeName", object.volume_name ? Json(*object.volume_name) : Json(nullptr)}});
        }
        if (source->native_keys_by_id) {
            for (const auto &[id, key] : *source->native_keys_by_id) {
                if (auto found = page_ids.find(key); found != page_ids.end()) {
                    rows[found->second]["id"] = id;
                    page_ids.erase(found);
                }
            }
            if (!page_ids.empty())
                return std::unexpected(
                    Error{"conversion_source_invalid", "Source object is missing its workspace identity"});
        }
        return Json{{"sourceFormat", summary.format_id},
                    {"documentId", source->reference.document_id.empty() ? summary.id : source->reference.document_id},
                    {"profileIds", std::move(applicable)},
                    {"offset", offset},
                    {"total", total},
                    {"items", std::move(rows)}};
    } catch (const Json::exception &) {
        return std::unexpected(Error{"invalid_request", "Malformed conversion inspection request"});
    }
}
} // namespace axk::app::conversion_internal
