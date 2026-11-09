#include "formats/tx16w/admission.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include "axklib/formats/tx16w/dependencies.hpp"
#include "axklib/utf8.hpp"
#include "formats/tx16w/session_state.hpp"
#include "image_sessions_internal.hpp"

namespace axk::app::formats::tx16w {
namespace {

Result<std::vector<ImageSourceRef>> nearby(const Sandbox &sandbox, const ImageSourceRef &source,
                                           const axk::tx16w::Document &document, std::string_view owner_id,
                                           const CancellationToken &cancellation) {
    const auto candidates = nearby_candidates(sandbox, source, cancellation);
    if (!candidates)
        return std::unexpected(candidates.error());
    std::vector<ImageSourceRef> result;
    for (const auto &candidate : *candidates) {
        if (auto active = cancellation.check(); !active)
            return std::unexpected(Error{"cancelled", "Companion discovery cancelled"});
        const auto admitted =
            admit_image_sources(sandbox, nullptr, nullptr, std::array{candidate}, owner_id, cancellation);
        if (!admitted)
            continue;
        const auto decoded = axk::tx16w::Document::open(admitted->native_sources, cancellation);
        if (!decoded)
            continue;
        if (axk::tx16w::detail::supplies_requirement(document, **decoded) ||
            axk::tx16w::detail::supplies_requirement(**decoded, document))
            result.push_back(candidate);
        if (result.size() > 31U)
            return std::unexpected(
                Error{"invalid_companion_sources", "Too many matching companions; select sources explicitly"});
    }
    if (auto active = cancellation.check(); !active)
        return std::unexpected(Error{"cancelled", "Companion discovery cancelled"});
    return result;
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
    std::vector<ImageSourceRef> result;
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
        result.push_back({source.root_id, entry.relative_path, ImageSourceKind::file});
    }
    return result;
}

NativeDocumentAdmission admit_document(std::shared_ptr<const axk::tx16w::Document> document,
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
    if (result.primary && result.primary->reference.kind == ImageSourceKind::file) {
        result.companions.can_search_nearby = true;
        result.find_companions = [reference = result.primary->reference,
                                  retained = std::move(document)](const Sandbox &sandbox, std::string_view owner_id,
                                                                  const CancellationToken &token) {
            return nearby(sandbox, reference, *retained, owner_id, token);
        };
    }
    return result;
}

} // namespace axk::app::formats::tx16w
