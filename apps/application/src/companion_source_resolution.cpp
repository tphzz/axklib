#include "companion_source_resolution.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "axklib/application/image_session_contracts.hpp"
#include "axklib/formats/tx16w/dependencies.hpp"
#include "axklib/formats/typhoon/companions.hpp"
#include "axklib/utf8.hpp"
#include "formats/tx16w/admission.hpp"

namespace axk::app::conversion_internal {
namespace {
using Json = nlohmann::json;
struct Identity {
    std::optional<std::filesystem::path> path;
    std::string upload_id;
};
Result<Identity> identity(const Json &source, const Sandbox &sandbox) {
    if (!source.is_object() || source.size() != 2U)
        return std::unexpected(Error{"invalid_request", "Select FILE or UPLOAD sources"});
    if (source.at("kind") == "UPLOAD") {
        const auto &upload = source.at("upload");
        if (!upload.is_object() || upload.size() != 1U)
            return std::unexpected(Error{"invalid_request", "Malformed upload reference"});
        return Identity{{}, upload.at("uploadId").get<std::string>()};
    }
    if (source.at("kind") != "FILE" || !source.at("file").is_object() || source.at("file").size() != 2U)
        return std::unexpected(Error{"invalid_request", "Companion resolution requires FILE or UPLOAD sources"});
    const auto parsed = image_source_ref_from_json(source);
    if (!parsed)
        return std::unexpected(parsed.error());
    const auto path = sandbox.resolve_file({parsed->root_id, parsed->relative_path});
    if (!path)
        return std::unexpected(path.error());
    return Identity{*path, {}};
}
bool same(const Identity &left, const Identity &right) {
    if (!left.path || !right.path)
        return !left.path && !right.path && left.upload_id == right.upload_id;
    std::error_code error;
    return *left.path == *right.path || std::filesystem::equivalent(*left.path, *right.path, error);
}
struct Candidate {
    Json reference;
    native::SourceMember member;
    std::shared_ptr<const native::Document> document;
    bool selected{};
};
} // namespace

Result<Json> resolve_companion_sources(const Json &input, const native::FormatRegistry &formats, const Sandbox &sandbox,
                                       UploadStore &uploads, ImageSessionManager &images,
                                       const OperationContext &context) {
    try {
        if (const auto active = context.cancellation.check(); !active)
            return std::unexpected(Error{"operation_cancelled", "Companion resolution cancelled"});
        if (!input.is_object() || !input.contains("sources"))
            return std::unexpected(Error{"invalid_request", "Select sources for companion resolution"});
        for (const auto &[key, value] : input.items()) {
            static_cast<void>(value);
            if (key != "sources" && key != "candidates" && key != "exclusions" && key != "searchNearby" &&
                key != "formatId")
                return std::unexpected(Error{"invalid_request", "Unknown companion resolution field"});
        }
        const auto &selected = input.at("sources");
        const auto format = input.value("formatId", std::string{"yamaha.tx16w"});
        if (format != "yamaha.tx16w" && format != "nuedge.typhoon")
            return std::unexpected(Error{"invalid_request", "Unknown companion resolution format"});
        const auto supplied = input.value("candidates", Json::array());
        const auto excluded = input.value("exclusions", Json::array());
        if (!selected.is_array() || selected.empty() || selected.size() > 32U || !supplied.is_array() ||
            supplied.size() > 1024U || !excluded.is_array() || excluded.size() > 1024U)
            return std::unexpected(Error{"invalid_request", "Source or discovery candidate count exceeds its limit"});
        std::vector<Identity> exclusions;
        for (const auto &reference : excluded) {
            auto resolved = identity(reference, sandbox);
            if (!resolved)
                return std::unexpected(resolved.error());
            exclusions.push_back(std::move(*resolved));
        }
        std::vector<Candidate> candidates;
        std::vector<Identity> visited;
        std::uint64_t candidate_bytes{};
        auto admit = [&](const Json &reference, bool explicit_source) -> Result<void> {
            if (const auto active = context.cancellation.check(); !active)
                return std::unexpected(Error{"operation_cancelled", "Companion resolution cancelled"});
            auto resolved = identity(reference, sandbox);
            if (!resolved)
                return std::unexpected(resolved.error());
            if (std::ranges::any_of(visited, [&](const auto &item) { return same(item, *resolved); }) ||
                (!explicit_source &&
                 std::ranges::any_of(exclusions, [&](const auto &item) { return same(item, *resolved); })))
                return {};
            visited.push_back(*resolved);
            // Apply input budgets before admission or decoding, including uploads.
            std::uint64_t size{};
            if (resolved->path) {
                auto file = sandbox.open_file({reference.at("file").at("rootId").get<std::string>(),
                                               reference.at("file").at("relativePath").get<std::string>()});
                if (!file)
                    return std::unexpected(file.error());
                size = file->size;
            } else {
                auto upload = uploads.inspect({resolved->upload_id}, context.owner_id);
                if (!upload)
                    return std::unexpected(upload.error());
                size = upload->declared_size;
            }
            if (size > 4U * 1024U * 1024U)
                return std::unexpected(Error{"companion_discovery_limit", "Companion source exceeds 4 MiB"});
            if (!explicit_source) {
                if (size > 64U * 1024U * 1024U - candidate_bytes)
                    return std::unexpected(Error{"companion_discovery_limit", "Companion discovery exceeds 64 MiB"});
                candidate_bytes += size;
            }
            auto opened = open_source({{"kind", "SOURCES"}, {"sources", Json::array({reference})}}, formats, format,
                                      sandbox, uploads, images, context, 4U * 1024U * 1024U);
            if (!opened) {
                if (opened.error().code == "source_byte_limit")
                    return std::unexpected(Error{"companion_discovery_limit", opened.error().message});
                if (!explicit_source && (opened.error().code == "conversion_source_invalid" ||
                                         opened.error().code == "conversion_source_ambiguous" ||
                                         opened.error().code == "image_source_unrecognized"))
                    return {};
                return std::unexpected(opened.error());
            }
            auto document = opened->document;
            if (!document || document->sources()->members().size() != 1U)
                return std::unexpected(Error{"invalid_companion_sources", "Expected one TX16W source member"});
            auto member = document->sources()->members().front();
            if (member.reader->size() != size)
                return std::unexpected(Error{"image_source_changed", "Companion source changed during admission"});
            member.id = "companion-" + std::to_string(candidates.size());
            candidates.push_back({reference, std::move(member), std::move(document), explicit_source});
            return {};
        };
        for (const auto &reference : selected)
            if (const auto result = admit(reference, true); !result)
                return std::unexpected(result.error());
        std::vector<native::SourceMember> members;
        for (const auto &candidate : candidates)
            members.push_back(candidate.member);
        auto selected_sources = native::SourceSet::create(members);
        if (!selected_sources)
            return std::unexpected(Error{"invalid_companion_sources", selected_sources.error().message});
        auto seed = formats.open_one(*selected_sources, format, context.cancellation);
        if (!seed)
            return std::unexpected(Error{seed.error().code == ErrorCode::operation_cancelled
                                             ? "operation_cancelled"
                                             : "invalid_companion_sources",
                                         seed.error().message});
        const auto needs_companions = !(*seed)->summary().source_requirements.empty();
        Json nearby = needs_companions ? supplied : Json::array();
        const auto supplied_count = nearby.size();
        if (needs_companions && input.value("searchNearby", true) && selected.front().at("kind") == "FILE") {
            auto first = image_source_ref_from_json(selected.front());
            if (!first)
                return std::unexpected(first.error());
            auto found = formats::tx16w::nearby_candidates(sandbox, *first, context.cancellation);
            if (!found)
                return std::unexpected(found.error());
            for (const auto &reference : *found)
                nearby.push_back(image_source_ref_json(reference));
        }
        if (nearby.size() > 1024U)
            return std::unexpected(Error{"companion_discovery_limit", "Too many companion discovery candidates"});
        for (std::size_t i = 0; i < nearby.size(); ++i)
            if (const auto result = admit(nearby.at(i), false); !result) {
                // A sandbox-listed neighbor may disappear or be an unrelated malformed image.
                // Never turn an invalid supplied reference, resource limit or stale read into a match.
                if (i >= supplied_count &&
                    (result.error().code == "entry_not_found" || result.error().code == "image_open_failed" ||
                     result.error().code == "filesystem_invalid"))
                    continue;
                return std::unexpected(result.error());
            }
        std::vector<native::SourceMember> neighbors;
        std::vector<std::shared_ptr<const axk::tx16w::Document>> documents;
        std::vector<std::shared_ptr<const axk::typhoon::Document>> typhoon_docs;
        for (const auto &candidate : candidates) {
            if (candidate.selected)
                continue;
            neighbors.push_back(candidate.member);
            if (format == "yamaha.tx16w")
                documents.push_back(std::dynamic_pointer_cast<const axk::tx16w::Document>(candidate.document));
            else
                typhoon_docs.push_back(std::dynamic_pointer_cast<const axk::typhoon::Document>(candidate.document));
        }
        const auto group = [&]() -> axk::Result<axk::tx16w::detail::CompanionGroup> {
            if (const auto yamaha = std::dynamic_pointer_cast<const axk::tx16w::Document>(*seed))
                return axk::tx16w::detail::resolve_companions(*yamaha, members, neighbors, documents,
                                                              context.cancellation);
            const auto ty = std::dynamic_pointer_cast<const axk::typhoon::Document>(*seed);
            if (!ty)
                return std::unexpected(
                    make_error(ErrorCode::invalid_argument, ErrorCategory::object, "Unexpected native format"));
            const auto matches = axk::typhoon::find_companions(*ty, typhoon_docs, context.cancellation);
            if (!matches)
                return std::unexpected(matches.error());
            axk::tx16w::detail::CompanionGroup result;
            for (const auto i : matches->candidates)
                result.member_ids.push_back(neighbors.at(i).id);
            result.ambiguous_requirements.assign(matches->ambiguous.begin(), matches->ambiguous.end());
            return result;
        }();
        if (!group)
            return std::unexpected(Error{group.error().code == ErrorCode::operation_cancelled
                                             ? "operation_cancelled"
                                             : "invalid_companion_sources",
                                         group.error().message});
        members.clear();
        Json result_members = Json::array();
        for (const auto &candidate : candidates) {
            const bool included = candidate.selected ||
                                  std::ranges::find(group->member_ids, candidate.member.id) != group->member_ids.end();
            if (!included)
                continue;
            members.push_back(candidate.member);
            const auto path = text::path_from_utf8(candidate.member.label);
            const auto name = path ? text::path_to_utf8(path->filename()) : candidate.member.label;
            result_members.push_back({{"source", candidate.reference},
                                      {"name", name},
                                      {"origin", candidate.selected ? "SELECTED" : "AUTO_ADDED"}});
        }
        auto sources = native::SourceSet::create(std::move(members));
        if (!sources)
            return std::unexpected(Error{"invalid_companion_sources", sources.error().message});
        auto document = [&]() -> axk::Result<std::shared_ptr<const native::Document>> {
            if (format == "nuedge.typhoon")
                return axk::typhoon::Document::open(*sources, context.cancellation);
            return axk::tx16w::Document::open(*sources, context.cancellation, {}, group->ambiguous_requirements);
        }();
        if (!document)
            return std::unexpected(Error{document.error().code == ErrorCode::operation_cancelled
                                             ? "operation_cancelled"
                                             : "invalid_companion_sources",
                                         document.error().message});
        Json requirements = Json::array();
        for (const auto &item : (*document)->summary().source_requirements) {
            const auto state =
                std::ranges::find(group->ambiguous_requirements, item.name) != group->ambiguous_requirements.end()
                    ? "AMBIGUOUS"
                : item.state == native::DependencyState::missing     ? "MISSING"
                : item.state == native::DependencyState::ambiguous   ? "AMBIGUOUS"
                : item.state == native::DependencyState::unsupported ? "UNSUPPORTED"
                                                                     : "INVALID";
            requirements.push_back({{"name", item.name}, {"state", state}});
        }
        return Json{{"members", std::move(result_members)}, {"requirements", std::move(requirements)}};
    } catch (const Json::exception &) {
        return std::unexpected(Error{"invalid_request", "Malformed companion resolution request"});
    }
}
} // namespace axk::app::conversion_internal
