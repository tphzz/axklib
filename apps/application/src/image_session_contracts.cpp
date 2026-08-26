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
        if (kind == "AXK_OBJECT_DIRECTORY") {
            const auto &directory = reference.at("directory");
            return ImageSourceRef{directory.at("rootId").get<std::string>(),
                                  directory.at("relativePath").get<std::string>(),
                                  ImageSourceKind::axk_object_directory};
        }
    } catch (const nlohmann::json::exception &) {
        return std::unexpected(Error{"invalid_request", "source must be one FILE or AXK_OBJECT_DIRECTORY reference"});
    }
    return std::unexpected(Error{"invalid_request", "image source kind is unsupported"});
}

nlohmann::json axk::app::image_source_ref_json(const ImageSourceRef &source) {
    if (source.kind == ImageSourceKind::file) {
        return {{"kind", "FILE"}, {"file", {{"rootId", source.root_id}, {"relativePath", source.relative_path}}}};
    }
    return {{"kind", "AXK_OBJECT_DIRECTORY"},
            {"directory", {{"rootId", source.root_id}, {"relativePath", source.relative_path}}}};
}

nlohmann::json axk::app::image_session_summary_json(const ImageSessionSummary &summary) {
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
    return {{"imageId", summary.image_id},
            {"revision", summary.revision},
            {"source", image_source_ref_json(summary.source)},
            {"companionSources", std::move(companion_sources)},
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
