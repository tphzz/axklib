#include "formats/typhoon/session_companions.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "axklib/formats/typhoon/companions.hpp"
#include "formats/tx16w/admission.hpp"

namespace axk::app::formats::typhoon {
namespace {
using Doc = axk::typhoon::Document;
Result<std::vector<ImageSourceRef>> nearby(const Sandbox &sandbox, const ImageSourceRef &seed, const Doc &doc,
                                           const std::vector<std::filesystem::path> &selected, std::string_view owner,
                                           const CancellationToken &token) {
    const auto entries = tx16w::nearby_candidates(sandbox, seed, token);
    if (!entries)
        return std::unexpected(entries.error());
    std::vector<ImageSourceRef> refs;
    std::vector<std::shared_ptr<const Doc>> docs;
    auto visited = selected;
    for (const auto &ref : *entries) {
        if (!token.check())
            return std::unexpected(Error{"operation_cancelled", "Companion discovery cancelled"});
        const auto path = sandbox.resolve_file({ref.root_id, ref.relative_path});
        if (!path || std::ranges::any_of(visited, [&](const auto &seen) {
                std::error_code ec;
                return seen == *path || std::filesystem::equivalent(seen, *path, ec);
            }))
            continue;
        visited.push_back(*path);
        const auto source = admit_image_sources(sandbox, nullptr, nullptr, std::array{ref}, owner, token, nullptr,
                                                nullptr, 4U * 1024U * 1024U);
        if (!source)
            continue;
        if (auto disk = Doc::open(source->native_sources, token)) {
            docs.push_back(std::move(*disk));
            refs.push_back(ref);
        }
    }
    if (!token.check())
        return std::unexpected(Error{"operation_cancelled", "Companion discovery cancelled"});
    const auto matches = axk::typhoon::find_companions(doc, docs, token);
    if (!matches)
        return std::unexpected(Error{matches.error().code == ErrorCode::operation_cancelled
                                         ? "operation_cancelled"
                                         : "invalid_companion_sources",
                                     matches.error().message});
    if (!matches->ambiguous.empty())
        return std::unexpected(Error{"companion_sources_ambiguous", "Multiple nearby disks match " +
                                                                        *matches->ambiguous.begin() +
                                                                        ". Add the companion disk explicitly."});
    if (matches->candidates.empty())
        return std::unexpected(Error{"companion_segment_not_found", "No matching Typhoon disk found nearby"});
    std::vector<ImageSourceRef> result;
    for (const auto i : matches->candidates)
        result.push_back(refs.at(i));
    return result;
}
} // namespace

Result<void> check_extension(const native::Document &previous, const Doc &next) {
    std::set<std::string> reached, members;
    for (const auto &obj : previous.catalog().objects) {
        if (!next.find(obj.id))
            return std::unexpected(Error{"document_membership_changed", "An existing Typhoon item disappeared"});
        reached.insert(obj.locations.front().member_id);
    }
    for (const auto &obj : next.catalog().objects)
        members.insert(obj.locations.front().member_id);
    if (members == reached)
        return {};
    // Each new disk must supply an item reachable from the previous disk set.
    std::map<std::string, std::set<std::string>> supplies;
    for (const auto &edge : next.catalog().relationships)
        if (edge.target_id) {
            const auto &from = next.find(edge.source_id)->location.member_id;
            const auto &to = next.find(*edge.target_id)->location.member_id;
            if (from != to)
                supplies[from].insert(to);
        }
    std::deque<std::string> queue(reached.begin(), reached.end());
    while (!queue.empty()) {
        const auto from = std::move(queue.front());
        queue.pop_front();
        for (const auto &to : supplies[from])
            if (reached.insert(to).second)
                queue.push_back(to);
    }
    if (reached != members)
        return std::unexpected(Error{"invalid_companion_sources", "A selected disk supplies no required Typhoon item"});
    return {};
}

void configure_companions(NativeDocumentAdmission &a, const AdmittedImageSources &sources,
                          std::shared_ptr<const Doc> doc) {
    a.companions.can_attach = true;
    for (const auto &req : doc->summary().source_requirements)
        a.companions.requirements.push_back(
            {req.name, req.state == native::DependencyState::ambiguous ? "AMBIGUOUS" : "MISSING", req.object_id});
    if (doc->summary().completeness == native::Completeness::invalid ||
        std::ranges::any_of(a.companions.requirements, [](const auto &r) { return r.state == "AMBIGUOUS"; }))
        a.companion_attachment_error = Error{"invalid_companion_sources", "Conflicting or invalid Typhoon items"};
    if (!a.primary || a.primary->reference.kind != ImageSourceKind::file || a.companions.requirements.empty())
        return;
    std::vector<std::filesystem::path> selected;
    for (const auto &m : sources.members)
        selected.push_back(m->canonical_path);
    a.companions.can_search_nearby = true;
    a.find_companions = [seed = a.primary->reference, retained = std::move(doc), paths = std::move(selected)](
                            const Sandbox &sandbox, std::string_view owner, const CancellationToken &token) {
        return nearby(sandbox, seed, *retained, paths, owner, token);
    };
}

native::FormatProvider scoped_provider(const SessionFormats &formats) {
    auto provider = axk::typhoon::provider();
    auto groups = formats.member_groups(provider.descriptor.id);
    if (groups.empty())
        return provider;
    provider.open = [scopes = std::move(groups)](auto sources, const CancellationToken &token)
        -> axk::Result<std::vector<std::shared_ptr<const native::Document>>> {
        std::vector<std::shared_ptr<const native::Document>> docs;
        for (const auto &group : scopes) {
            auto doc = Doc::open(sources, token, group);
            if (!doc)
                return std::unexpected(doc.error());
            docs.push_back(std::move(*doc));
        }
        return docs;
    };
    return provider;
}
} // namespace axk::app::formats::typhoon
