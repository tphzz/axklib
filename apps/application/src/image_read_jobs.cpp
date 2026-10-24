#include "image_read_jobs.hpp"
#include "axklib/application/image_session_contracts.hpp"
#include "image_sessions_internal.hpp"
#include "operation_diagnostics.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <set>
#include <string>
#include <utility>

namespace axk::app {
Result<std::string> ImageSessionManager::read_job_scope(const ImageDocumentRef &target, std::string_view owner_id,
                                                        std::uint64_t expected_revision) const {
    if (!expected_revision || target.document_id.empty())
        return std::unexpected(Error{"invalid_request", "a document and revision are required"});
    const std::scoped_lock lock{implementation_->mutex};
    const auto found = implementation_->sessions.find(target.image_id);
    if (found == implementation_->sessions.end() || found->second->owner_id != owner_id)
        return std::unexpected(session_error("image_not_found", "image session does not exist"));
    // Do not take access_mutex here: the scan being preempted owns it. The read
    // validates the document/revision under that lock before touching source bytes.
    // Including revision in this group prevents stale requests preempting a newer revision.
    return std::format("{}:{}", target.image_id, expected_revision);
}

Result<std::optional<ReadJobScheduling>> image_read_scheduling(ImageSessionManager &images,
                                                               const nlohmann::json &request,
                                                               const OperationContext &context, ImageReadJob kind) {
    try {
        const auto invalid = [] { return std::unexpected(Error{"invalid_request", "invalid audio read request"}); };
        const auto text = [](const auto &value) {
            return value.is_string() && !value.template get_ref<const std::string &>().empty() &&
                   value.template get_ref<const std::string &>().size() <= 512U;
        };
        const auto positive = [](const auto &value) {
            return value.is_number_unsigned() ? value.template get<std::uint64_t>() > 0U
                                              : value.is_number_integer() && value.template get<std::int64_t>() > 0;
        };
        if (!text(request.at("imageId")) || !text(request.at("documentId")) ||
            !positive(request.at("expectedRevision")) || !text(request.at("readIntent")))
            return invalid();
        const auto intent = request.at("readIntent").get<std::string>();
        ReadIntent priority;
        if (kind == ImageReadJob::preview) {
            if (!text(request.at("objectId")) || !positive(request.at("bins")))
                return invalid();
            const auto bins = request.at("bins").get<std::uint64_t>();
            if (bins == 0U || bins > 4096U)
                return invalid();
            if (intent == "SELECTED")
                priority = ReadIntent::selected;
            else if (intent == "REFINEMENT")
                priority = ReadIntent::refinement;
            else
                return invalid();
        } else {
            const auto &objects = request.at("objectIds");
            if (!objects.is_array() || objects.empty() || objects.size() > 256U)
                return invalid();
            std::set<std::string> unique;
            for (const auto &object : objects) {
                if (!text(object) || !unique.insert(object.get<std::string>()).second)
                    return invalid();
            }
            const auto window = request.value("sourceWindow", "PLAYBACK");
            if (window != "PLAYBACK" && window != "STORED")
                return invalid();
            if (intent == "PLAYBACK")
                priority = ReadIntent::playback;
            else if (intent == "PREFETCH")
                priority = ReadIntent::prefetch;
            else
                return invalid();
        }
        auto scope = images.read_job_scope(image_document_ref(request), context.owner_id,
                                           request.at("expectedRevision").get<std::uint64_t>());
        if (!scope)
            return std::unexpected(scope.error());
        return ReadJobScheduling{std::move(*scope), priority};
    } catch (const nlohmann::json::exception &) {
        return std::unexpected(Error{"invalid_request", "invalid audio read request"});
    }
}

Result<void> bind_preview_operation(OperationRegistry &registry, ImageSessionManager &images) {
    if (registry.is_implemented("images.preview"))
        return {};
    auto bound = registry.bind(
        "images.preview", [&images](const auto &request, const OperationContext &context) -> Result<nlohmann::json> {
            if (auto policy = image_read_scheduling(images, request, context, ImageReadJob::preview); !policy)
                return std::unexpected(policy.error());
            OperationDiagnosticSink diagnostics{context};
            const auto preview = images.preview(
                image_document_ref(request), context.owner_id, request.at("objectId").template get<std::string>(),
                request.at("bins").template get<std::size_t>(),
                request.at("expectedRevision").template get<std::uint64_t>(), context.cancellation, &diagnostics);
            if (!preview)
                return std::unexpected(preview.error());
            auto lanes = nlohmann::json::array();
            for (const auto &lane : preview->lanes) {
                auto bins = nlohmann::json::array();
                for (const auto &bin : lane.bins)
                    bins.push_back({{"minimum", bin.minimum}, {"maximum", bin.maximum}});
                lanes.push_back({{"role", lane.role},
                                 {"sourceObjectId", lane.source_object_id},
                                 {"sampleRate", lane.sample_rate},
                                 {"storedFrameCount", lane.stored_frame_count},
                                 {"sampleWidthBytes", lane.sample_width_bytes},
                                 {"nativeBitDepth", lane.native_bit_depth},
                                 {"sampleRateApproximate", lane.sample_rate_approximate},
                                 {"playbackStartFrame", lane.playback_start_frame},
                                 {"playbackLengthFrames", lane.playback_length_frames},
                                 {"loopStartFrame", lane.loop_start_frame},
                                 {"loopLengthFrames", lane.loop_length_frames},
                                 {"bins", std::move(bins)}});
            }
            return nlohmann::json{{"objectId", preview->object_id},     {"imageId", preview->image_id},
                                  {"documentId", preview->document_id}, {"revision", preview->revision},
                                  {"warnings", preview->warnings},      {"lanes", std::move(lanes)}};
        });
    if (!bound)
        return bound;
    return registry.bind_read_scheduling("images.preview", [&images](const auto &request, const auto &context) {
        return image_read_scheduling(images, request, context, ImageReadJob::preview);
    });
}
} // namespace axk::app
