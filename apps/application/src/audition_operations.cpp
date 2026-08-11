#include "axklib/application/audition_operations.hpp"

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/application/image_sessions.hpp"

namespace {

using Json = nlohmann::json;

} // namespace

axk::app::Result<void> axk::app::bind_audition_operations(OperationRegistry &registry, ImageSessionManager &images) {
    if (registry.is_implemented("auditions.prepare"))
        return {};
    return registry.bind(
        "auditions.prepare", [&images](const Json &request, const OperationContext &context) -> Result<Json> {
            const auto image = request.find("imageId");
            const auto objects = request.find("objectIds");
            if (image == request.end() || objects == request.end() || !image->is_string() || !objects->is_array()) {
                return std::unexpected(Error{"invalid_request", "imageId and objectIds are required"});
            }
            std::vector<std::string> object_ids;
            object_ids.reserve(objects->size());
            for (const auto &object : *objects) {
                if (!object.is_string())
                    return std::unexpected(Error{"invalid_request", "audition object IDs must be strings"});
                object_ids.push_back(object.get<std::string>());
            }
            if (context.progress != nullptr) {
                context.progress->report({axk::ProgressPhase::reading, 0U, object_ids.size(),
                                          "Preparing bounded audio sources", std::nullopt});
            }
            auto audition = images.prepare_audition(image->get_ref<const std::string &>(), context.owner_id, object_ids,
                                                    context.cancellation);
            if (!audition)
                return std::unexpected(audition.error());
            if (context.progress != nullptr) {
                context.progress->report({axk::ProgressPhase::reading, object_ids.size(), object_ids.size(),
                                          "Audio sources ready", std::nullopt});
            }
            Json clips = Json::array();
            for (const auto &clip : audition->clips) {
                Json lanes = Json::array();
                for (const auto &lane : clip.lanes) {
                    lanes.push_back({{"role", lane.role},
                                     {"sourceObjectId", lane.source_object_id},
                                     {"sampleRate", lane.sample_rate},
                                     {"sampleWidthBytes", lane.sample_width_bytes},
                                     {"frameCount", lane.frame_count},
                                     {"contentOffsetBytes", lane.content_offset_bytes},
                                     {"wavSizeBytes", lane.wav_size_bytes},
                                     {"loopStartFrame", lane.loop_start_frame},
                                     {"loopLengthFrames", lane.loop_length_frames}});
                }
                clips.push_back({{"objectId", clip.object_id},
                                 {"loopMode", clip.loop_mode},
                                 {"loopModeLabel", clip.loop_mode_label},
                                 {"warnings", clip.warnings},
                                 {"lanes", std::move(lanes)}});
            }
            return Json{{"auditionId", audition->audition_id},
                        {"contentSizeBytes", audition->content_size_bytes},
                        {"clips", std::move(clips)}};
        });
}
