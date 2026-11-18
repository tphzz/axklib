#include "formats/tx16w/admission.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "axklib/formats/tx16w/dependencies.hpp"
#include "axklib/utf8.hpp"
#include "formats/tx16w/session_state.hpp"
#include "image_sessions_internal.hpp"
#include "session_formats.hpp"

namespace axk::app::formats::tx16w {
namespace {

Result<std::vector<ImageSourceRef>> nearby(const Sandbox &sandbox, const ImageSourceRef &source,
                                           const std::shared_ptr<const axk::tx16w::Document> &document,
                                           const std::set<std::string> &selected_ids,
                                           std::vector<std::filesystem::path> visited, std::string_view owner_id,
                                           const CancellationToken &cancellation) {
    const auto candidates = nearby_candidates(sandbox, source, cancellation);
    if (!candidates)
        return std::unexpected(candidates.error());
    std::vector<native::SourceMember> selected;
    for (const auto &member : document->sources()->members())
        if (selected_ids.contains(member.id))
            selected.push_back(member);
    std::vector<native::SourceMember> members;
    std::vector<std::shared_ptr<const axk::tx16w::Document>> documents;
    std::vector<ImageSourceRef> references;
    for (const auto &candidate : *candidates) {
        if (auto active = cancellation.check(); !active)
            return std::unexpected(Error{"cancelled", "Companion discovery cancelled"});
        const auto path = sandbox.resolve_file({candidate.root_id, candidate.relative_path});
        if (!path || std::ranges::any_of(visited, [&](const auto &seen) {
                std::error_code error;
                return seen == *path || std::filesystem::equivalent(seen, *path, error);
            }))
            continue;
        visited.push_back(*path);
        const auto admitted = admit_image_sources(sandbox, nullptr, nullptr, std::array{candidate}, owner_id,
                                                  cancellation, nullptr, nullptr, 4U * 1024U * 1024U);
        if (!admitted)
            continue;
        const auto decoded = axk::tx16w::Document::open(admitted->native_sources, cancellation);
        if (!decoded)
            continue;
        auto member = admitted->native_sources->members().front();
        member.id = std::to_string(references.size());
        members.push_back(std::move(member));
        documents.push_back(*decoded);
        references.push_back(candidate);
    }
    if (auto active = cancellation.check(); !active)
        return std::unexpected(Error{"cancelled", "Companion discovery cancelled"});
    const auto group = axk::tx16w::detail::resolve_companions(*document, selected, members, documents, cancellation);
    if (!group)
        return std::unexpected(Error{group.error().code == ErrorCode::operation_cancelled ? "operation_cancelled"
                                                                                          : "invalid_companion_sources",
                                     group.error().message});
    if (!group->ambiguous_requirements.empty())
        return std::unexpected(Error{"companion_sources_ambiguous", "Multiple nearby disks match " +
                                                                        group->ambiguous_requirements.front() +
                                                                        ". Add the companion disk explicitly."});
    std::vector<ImageSourceRef> matches;
    for (std::size_t i = 0; i < references.size(); ++i)
        if (std::ranges::find(group->member_ids, members[i].id) != group->member_ids.end())
            matches.push_back(references[i]);
    if (!matches.empty())
        return matches;
    return std::unexpected(
        Error{"companion_segment_not_found", "No matching disk found nearby. Add the companion disk explicitly."});
}

} // namespace

Result<std::vector<ImageSourceRef>> nearby_candidates(const Sandbox &sandbox, const ImageSourceRef &source,
                                                      const CancellationToken &cancellation) {
    auto path = text::path_from_utf8(source.relative_path);
    if (!path)
        return std::unexpected(Error{"invalid_companion_sources", "Source path is invalid"});
    auto listing =
        sandbox.scan_directory({source.root_id, text::path_to_utf8(path->parent_path())}, 1024U, cancellation);
    if (!listing)
        return std::unexpected(listing.error());
    std::vector<std::string> paths;
    std::uint64_t total_bytes{};
    for (const auto &entry : *listing) {
        if (const auto active = cancellation.check(); !active)
            return std::unexpected(Error{"operation_cancelled", "Companion discovery cancelled"});
        if (entry.kind != DirectoryEntryKind::file || entry.relative_path == source.relative_path || !entry.size ||
            *entry.size > 4U * 1024U * 1024U)
            continue;
        if (*entry.size > 64U * 1024U * 1024U - total_bytes)
            return std::unexpected(Error{"companion_discovery_limit", "Companion discovery exceeds 64 MiB"});
        total_bytes += *entry.size;
        paths.push_back(entry.relative_path);
    }
    std::vector<ImageSourceRef> result;
    for (const auto i : axk::tx16w::detail::nearby_order(source.relative_path, paths))
        result.push_back({source.root_id, std::move(paths[i]), ImageSourceKind::file});
    return result;
}

static NativeDocumentAdmission admit_document(std::shared_ptr<const axk::tx16w::Document> document,
                                              const AdmittedImageSources &sources) {
    NativeDocumentAdmission result;
    result.id = "document-" + document->summary().id;
    result.format = "yamaha.tx16w";
    result.native = std::make_unique<SessionState>(document);
    std::set<std::string> members;
    for (const auto &file : document->native_files())
        members.insert(file.location.member_id);
    for (const auto &member : sources.members) {
        if (!members.contains(member->id))
            continue;
        result.member_ids.push_back(member->id);
        if (!result.primary)
            result.primary = member;
        else
            result.companion_sources.push_back(member->reference);
    }
    result.companions.can_attach = true;
    for (const auto &requirement : document->summary().source_requirements)
        result.companions.requirements.push_back(
            {requirement.name,
             requirement.state == native::DependencyState::missing       ? "MISSING"
             : requirement.state == native::DependencyState::ambiguous   ? "AMBIGUOUS"
             : requirement.state == native::DependencyState::unsupported ? "UNSUPPORTED"
                                                                         : "INVALID",
             requirement.object_id});
    if (document->summary().completeness == native::Completeness::invalid ||
        std::ranges::any_of(result.companions.requirements,
                            [](const auto &item) { return item.state == "AMBIGUOUS" || item.state == "INVALID"; }))
        result.companion_attachment_error =
            Error{"invalid_companion_sources", "Companions contain conflicting or invalid native files"};
    if (result.primary && result.primary->reference.kind == ImageSourceKind::file &&
        !result.companions.requirements.empty()) {
        std::vector<std::filesystem::path> paths;
        for (const auto &member : sources.members)
            paths.push_back(member->canonical_path);
        result.companions.can_search_nearby = true;
        result.find_companions = [reference = result.primary->reference, selected = std::move(members),
                                  selected_paths = std::move(paths),
                                  retained = std::move(document)](const Sandbox &sandbox, std::string_view owner_id,
                                                                  const CancellationToken &token) {
            return nearby(sandbox, reference, retained, selected, selected_paths, owner_id, token);
        };
    }
    return result;
}

Result<void> register_format(SessionFormats &formats, const SessionFormatContext &context) {
    auto provider = axk::tx16w::provider();
    if (auto groups = formats.member_groups(provider.descriptor.id); !groups.empty())
        provider.open = [scopes = std::move(groups)](auto sources, const CancellationToken &token) {
            return axk::tx16w::detail::open_grouped_documents(std::move(sources), token, scopes);
        };
    return formats.add(
        std::move(provider), [&sources = context.sources](const auto &document) -> Result<NativeDocumentAdmission> {
            const auto typed = std::dynamic_pointer_cast<const axk::tx16w::Document>(document);
            if (!typed)
                return std::unexpected(Error{"document_invalid", "TX16W provider returned an incompatible document"});
            return admit_document(typed, sources);
        });
}

} // namespace axk::app::formats::tx16w
