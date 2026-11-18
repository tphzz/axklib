#include "axklib/inspection/disk.hpp"

#include <algorithm>
#include <utility>

namespace axk::inspection {
using namespace native;
namespace {
using Json = nlohmann::json;

std::string_view completeness(Completeness state) {
    switch (state) {
    case Completeness::complete:
        return "complete";
    case Completeness::incomplete:
        return "incomplete";
    case Completeness::invalid:
        return "invalid";
    }
    std::unreachable();
}
std::string_view dependency(DependencyState state) {
    switch (state) {
    case DependencyState::missing:
        return "missing";
    case DependencyState::ambiguous:
        return "ambiguous";
    case DependencyState::unsupported:
        return "unsupported";
    case DependencyState::invalid:
        return "invalid";
    }
    std::unreachable();
}
Json location_json(const SourceLocation &value) {
    return {{"memberId", value.member_id},
            {"rootId", value.root_id},
            {"entryId", value.entry_id},
            {"offset", value.offset},
            {"length", value.length}};
}
} // namespace

Result<DiskInspection> DiskInspection::inspect(std::shared_ptr<const SourceSet> sources, const FormatRegistry &formats,
                                               const CancellationToken &cancellation) {
    auto opened = formats.open(sources, {}, cancellation);
    if (!opened)
        return std::unexpected(opened.error());
    if (opened->selection_required)
        return std::unexpected(make_error(ErrorCode::invalid_argument, ErrorCategory::object,
                                          "More than one native format claims the same records"));
    for (const auto &candidate : opened->candidates) {
        if (candidate.state == RecognitionState::limit_exceeded || candidate.state == RecognitionState::invalid)
            return std::unexpected(make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                              candidate.diagnostics.empty() ? "Cannot inspect native records"
                                                                            : candidate.diagnostics.front().message));
    }
    DiskInspection result;
    result.sources_ = std::move(sources);
    result.opened_ = std::move(*opened);
    return result;
}

Json DiskInspection::summary() const {
    auto members = Json::array();
    for (const auto &member : sources_->members()) {
        auto roots = Json::array();
        for (const auto &root : member.roots)
            roots.push_back(root.id);
        members.push_back({{"id", member.id}, {"name", member.label}, {"rootIds", std::move(roots)}});
    }
    auto documents = Json::array();
    for (const auto &doc : opened_.documents) {
        const auto &value = doc->summary();
        auto requirements = Json::array();
        for (const auto &req : value.source_requirements)
            requirements.push_back({{"name", req.name}, {"state", dependency(req.state)}, {"objectId", req.object_id}});
        documents.push_back({{"id", value.id},
                             {"formatId", value.format_id},
                             {"profile", value.profile},
                             {"name", value.name},
                             {"completeness", completeness(value.completeness)},
                             {"sourceRequirements", std::move(requirements)},
                             {"objectCount", doc->catalog().objects.size()},
                             {"relationshipCount", doc->catalog().relationships.size()}});
    }
    return {{"members", std::move(members)}, {"documents", std::move(documents)}};
}

Result<const Document *> DiskInspection::find(std::string_view id) const {
    for (const auto &doc : opened_.documents)
        if (doc->summary().id == id)
            return doc.get();
    return std::unexpected(make_error(ErrorCode::object_missing, ErrorCategory::object, "Unknown native document ID"));
}

Result<Json> DiskInspection::objects(std::string_view id, std::size_t offset, std::size_t limit,
                                     const CancellationToken &cancellation) const {
    const auto doc = find(id);
    if (!doc)
        return std::unexpected(doc.error());
    const auto page = (*doc)->objects(offset, limit, cancellation);
    if (!page)
        return std::unexpected(page.error());
    auto rows = Json::array();
    for (const auto &obj : page->items) {
        auto locations = Json::array();
        for (const auto &loc : obj.locations)
            locations.push_back(location_json(loc));
        rows.push_back({{"id", obj.id}, {"type", obj.type}, {"name", obj.name}, {"locations", std::move(locations)}});
        if (obj.volume_name)
            rows.back()["volumeName"] = *obj.volume_name;
    }
    return Json{{"items", std::move(rows)}, {"total", page->total}, {"offset", offset}};
}

Result<Json> DiskInspection::relationships(std::string_view id, std::size_t offset, std::size_t limit,
                                           const CancellationToken &cancellation) const {
    const auto doc = find(id);
    if (!doc)
        return std::unexpected(doc.error());
    const auto page = (*doc)->relationships(offset, limit, cancellation);
    if (!page)
        return std::unexpected(page.error());
    auto rows = Json::array();
    for (const auto &edge : page->items) {
        rows.push_back({{"id", edge.id},
                        {"type", edge.type},
                        {"sourceObjectId", edge.source_id},
                        {"targetObjectId", edge.target_id},
                        {"candidateObjectIds", edge.candidates},
                        {"nativeReference", edge.native_reference},
                        {"resolution", edge.resolution == Resolution::resolved    ? "resolved"
                                       : edge.resolution == Resolution::ambiguous ? "ambiguous"
                                                                                  : "missing"}});
    }
    return Json{{"items", std::move(rows)}, {"total", page->total}, {"offset", offset}};
}
} // namespace axk::inspection
