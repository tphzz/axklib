#include "axklib/application/image_session_contracts.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace {

std::string_view floppy_set_status_name(axk::app::ImageFloppySetStatus status) {
    switch (status) {
    case axk::app::ImageFloppySetStatus::single:
        return "SINGLE";
    case axk::app::ImageFloppySetStatus::incomplete:
        return "INCOMPLETE";
    case axk::app::ImageFloppySetStatus::complete:
        return "COMPLETE";
    case axk::app::ImageFloppySetStatus::recovery:
        return "RECOVERY";
    }
    return "SINGLE";
}

} // namespace

axk::app::Result<axk::app::ImageSourceRef> axk::app::image_source_ref_from_json(const nlohmann::json &reference) {
    try {
        const auto kind = reference.at("kind").get<std::string>();
        if (kind == "FILE") {
            const auto &file = reference.at("file");
            return ImageSourceRef{file.at("rootId").get<std::string>(), file.at("relativePath").get<std::string>(),
                                  ImageSourceKind::file};
        }
        if (kind == "DIRECTORY") {
            const auto &directory = reference.at("directory");
            return ImageSourceRef{directory.at("rootId").get<std::string>(),
                                  directory.at("relativePath").get<std::string>(), ImageSourceKind::directory};
        }
        if (kind == "DEVICE") {
            const auto &device = reference.at("device");
            const auto session = device.at("sessionId").get<std::string>();
            const auto target = device.at("targetId").get<std::string>();
            if (session.empty() || target.empty() || reference.size() != 2U || device.size() != 2U)
                return std::unexpected(Error{"invalid_request", "device source requires an owned session and target"});
            return ImageSourceRef{{}, {}, ImageSourceKind::device, session, target};
        }
    } catch (const nlohmann::json::exception &) {
        return std::unexpected(Error{"invalid_request", "source must be one FILE, DIRECTORY or DEVICE reference"});
    }
    return std::unexpected(Error{"invalid_request", "image source kind is unsupported"});
}

nlohmann::json axk::app::image_source_ref_json(const ImageSourceRef &source) {
    if (source.kind == ImageSourceKind::device)
        return {{"kind", "DEVICE"},
                {"device", {{"sessionId", source.device_session_id}, {"targetId", source.device_target_id}}}};
    if (source.kind == ImageSourceKind::file) {
        return {{"kind", "FILE"}, {"file", {{"rootId", source.root_id}, {"relativePath", source.relative_path}}}};
    }
    return {{"kind", "DIRECTORY"}, {"directory", {{"rootId", source.root_id}, {"relativePath", source.relative_path}}}};
}

axk::app::ImageDocumentRef axk::app::image_document_ref(const nlohmann::json &request) {
    return {request.at("imageId").get<std::string>(), request.at("documentId").get<std::string>()};
}

axk::app::Result<axk::app::ImageOpenSources> axk::app::image_open_sources_from_json(const nlohmann::json &request) {
    try {
        if (!request.is_object() || request.contains("source"))
            return std::unexpected(Error{"invalid_request", "Use an explicit sources array"});
        const auto &sources = request.at("sources");
        if (!sources.is_array() || sources.empty() || sources.size() > 32U)
            return std::unexpected(Error{"invalid_request", "Choose between 1 and 32 sources"});
        ImageOpenSources result;
        for (const auto &value : sources) {
            auto source = image_source_ref_from_json(value);
            if (!source)
                return std::unexpected(source.error());
            result.sources.push_back(std::move(*source));
        }
        if (request.contains("selectedFormats"))
            result.selected_formats = request.at("selectedFormats").get<std::vector<std::string>>();
        return result;
    } catch (const nlohmann::json::exception &) {
        return std::unexpected(Error{"invalid_request", "sources must contain explicit source references"});
    }
}

nlohmann::json axk::app::image_document_summary_json(const ImageDocumentSummary &summary) {
    auto requirements = nlohmann::json::array();
    for (const auto &item : summary.companions.requirements)
        requirements.push_back({{"name", item.name},
                                {"state", item.state},
                                {"objectId", item.object_id ? nlohmann::json(*item.object_id) : nlohmann::json{}}});
    nlohmann::json companion_sources = nlohmann::json::array();
    for (const auto &source : summary.companion_sources)
        companion_sources.push_back(image_source_ref_json(source));
    nlohmann::json floppy_set;
    if (summary.floppy_set) {
        nlohmann::json members = nlohmann::json::array();
        for (const auto &member : summary.floppy_set->members)
            members.push_back({{"index", member.index}, {"label", member.label}, {"marker", member.marker}});
        floppy_set = {{"status", floppy_set_status_name(summary.floppy_set->status)},
                      {"setLabel", summary.floppy_set->set_label},
                      {"members", std::move(members)},
                      {"nextRequiredIndex", summary.floppy_set->next_required_index
                                                ? nlohmann::json(*summary.floppy_set->next_required_index)
                                                : nlohmann::json{}}};
    }
    return {{"id", summary.id},
            {"formatId", summary.format_id},
            {"profile", summary.profile},
            {"name", summary.name},
            {"completeness", summary.completeness},
            {"sourceMemberIds", summary.source_member_ids},
            {"companionSources", std::move(companion_sources)},
            {"companions",
             {{"canAttach", summary.companions.can_attach},
              {"canSearchNearby", summary.companions.can_search_nearby},
              {"requirements", std::move(requirements)}}},
            {"floppySet", std::move(floppy_set)},
            {"format", summary.format},
            {"availableOperations", summary.available_operations},
            {"rootCount", summary.root_count},
            {"objectCount", summary.object_count},
            {"relationshipCount", summary.relationship_count},
            {"validation",
             {{"valid", summary.validation.valid()},
              {"infoCount", summary.validation.info_count},
              {"warningCount", summary.validation.warning_count},
              {"errorCount", summary.validation.error_count}}}};
}

nlohmann::json axk::app::image_session_summary_json(const ImageSessionSummary &summary) {
    auto sources = nlohmann::json::array();
    for (const auto &source : summary.sources) {
        auto diagnostics = nlohmann::json::array();
        for (const auto &issue : source.diagnostics)
            diagnostics.push_back(
                {{"code", issue.code}, {"severity", issue.severity}, {"message", issue.message}, {"path", issue.path}});
        sources.push_back({{"id", source.id},
                           {"source", image_source_ref_json(source.source)},
                           {"format", source.format},
                           {"filesystemRootIds", source.filesystem_root_ids},
                           {"diagnostics", std::move(diagnostics)}});
    }
    auto documents = nlohmann::json::array();
    for (const auto &document : summary.documents)
        documents.push_back(image_document_summary_json(document));
    auto candidates = nlohmann::json::array();
    for (const auto &candidate : summary.discovery.candidates)
        candidates.push_back({{"formatId", candidate.format_id},
                              {"state", candidate.state},
                              {"sourceMemberIds", candidate.source_member_ids},
                              {"diagnostics", candidate.diagnostics}});
    return {{"imageId", summary.image_id},
            {"revision", summary.revision},
            {"sources", std::move(sources)},
            {"documents", std::move(documents)},
            {"discovery",
             {{"selectionRequired", summary.discovery.selection_required}, {"candidates", std::move(candidates)}}}};
}
