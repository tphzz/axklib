#include "formats/typhoon/admission.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "axklib/formats/typhoon/document.hpp"
#include "formats/typhoon/session_companions.hpp"
#include "session_formats.hpp"

namespace axk::app::formats::typhoon {
namespace {
class State final : public NativeDocumentState, public NativeAudioProvider {
  public:
    explicit State(std::shared_ptr<const axk::typhoon::Document> doc) : doc_(std::move(doc)) {
        native_document = doc_;
        index_native_catalog();
    }
    std::string_view format_id() const noexcept override { return doc_->summary().format_id; }
    std::string_view profile() const noexcept override { return doc_->summary().profile; }
    std::vector<std::string> available_operations(bool, bool) const override {
        return {"images.content",           "images.objects", "images.relationships",
                "images.validation.issues", "images.preview", "auditions.prepare"};
    }
    NativeFilesystemBindings filesystem_bindings() const override { return {}; }
    Result<void> preserve_identity(const NativeDocumentState &old) override {
        if (old.format_id() != format_id())
            return std::unexpected(Error{"native_format_changed", "Native format changed"});
        return check_extension(*old.native_document, *doc_);
    }
    const NativeAudioProvider *audio() const noexcept override { return this; }
    Result<nlohmann::ordered_json> object_detail(const ImageDocumentContext &session,
                                                 std::string_view id) const override {
        auto item = doc_->find(id);
        if (!item)
            return std::unexpected(Error{"object_not_found", "Unknown Typhoon item"});
        using Json = nlohmann::ordered_json;
        const auto &loc = item->location;
        Json details = {{"itemVersion", item->version}, {"referenceCount", item->references.size()}};
        const auto reference = [&](std::size_t index) {
            const auto &ref = item->references.at(index);
            return Json{{"name", ref.name},
                        {"version", ref.version},
                        {"diskLabel", ref.disk},
                        {"targetObjectId", ref.target ? Json(*ref.target) : Json(nullptr)}};
        };
        if (item->type == "voice") {
            auto &rows = details["regions"] = Json::array();
            for (const auto &r : item->regions)
                rows.push_back({{"keyLow", r.low},
                                {"keyHigh", r.high},
                                {"velocityLow", r.velocity_low},
                                {"velocityHigh", r.velocity_high},
                                {"wave", reference(r.reference)}});
        } else if (item->type == "performance") {
            auto &rows = details["parts"] = Json::array();
            for (const auto &part : item->parts)
                rows.push_back({{"nativeChannel", part.channel}, {"voice", reference(part.reference)}});
        }
        if (item->wave) {
            const auto &wave = *item->wave;
            details["wave"] = {{"frames", wave.frames},
                               {"sampleRate", wave.sample_rate},
                               {"bits", wave.bits},
                               {"rootKey", wave.root_key ? Json(*wave.root_key) : Json(nullptr)},
                               {"loopStart", wave.loop ? Json(wave.loop->first) : Json(nullptr)},
                               {"loopEnd", wave.loop ? Json(wave.loop->second) : Json(nullptr)}};
        }
        return Json{{"imageId", session.image_id},
                    {"documentId", session.document_id},
                    {"revision", session.revision},
                    {"formatId", format_id()},
                    {"profile", profile()},
                    {"objectId", item->id},
                    {"type", item->type},
                    {"name", item->name},
                    {"details", std::move(details)},
                    {"locations", Json::array({{{"memberId", loc.member_id},
                                                {"rootId", loc.root_id},
                                                {"entryId", loc.entry_id},
                                                {"offset", loc.offset},
                                                {"length", loc.length}}})}};
    }
    Result<native::PcmSource> prepare_source(std::string_view id, native::PcmReadWindow) const override {
        auto item = doc_->find(id);
        if (!item || !item->wave)
            return std::unexpected(Error{"audition_unsupported", "Select a decoded Typhoon Wave"});
        auto &w = *item->wave;
        const auto start = w.loop ? w.loop->first : 0U, length = w.loop ? w.loop->second - start + 1U : 0U;
        native::PcmSource source;
        source.looping = w.loop.has_value();
        source.members.push_back({.object_id = item->id,
                                  .role = "MONO",
                                  .output_width = 2U,
                                  .native_bit_depth = w.bits,
                                  .sample_rate = w.sample_rate,
                                  .stored_frame_count = w.frames,
                                  .playback_length_frames = w.frames,
                                  .source_loop_start = start,
                                  .source_loop_length = length,
                                  .frame_count = w.frames,
                                  .loop_start = start,
                                  .loop_length = length});
        return source;
    }
    Result<std::vector<std::byte>> read_member_pcm(const native::PcmMember &member, std::uint64_t first,
                                                   std::size_t count, const CancellationToken &token) const override {
        if (first > UINT32_MAX || count > UINT32_MAX)
            return std::unexpected(Error{"audio_read_failed", "Invalid Wave window"});
        auto frames =
            doc_->pcm(member.object_id, static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(count), token);
        if (!frames)
            return std::unexpected(Error{frames.error().code == ErrorCode::operation_cancelled ? "operation_cancelled"
                                                                                               : "audio_read_failed",
                                         frames.error().message});
        std::vector<std::byte> out;
        out.reserve(frames->size() * 2);
        for (const auto frame : *frames) {
            const auto x = static_cast<std::uint16_t>(frame);
            out.push_back(static_cast<std::byte>(x & 255U));
            out.push_back(static_cast<std::byte>(x >> 8U));
        }
        return out;
    }

  private:
    std::shared_ptr<const axk::typhoon::Document> doc_;
};
} // namespace
Result<void> register_format(SessionFormats &formats, const SessionFormatContext &context) {
    return formats.add(scoped_provider(formats),
                       [&sources = context.sources](const auto &doc) -> Result<NativeDocumentAdmission> {
                           auto typed = std::dynamic_pointer_cast<const axk::typhoon::Document>(doc);
                           if (!typed)
                               return std::unexpected(Error{"document_invalid", "Invalid Typhoon document"});
                           NativeDocumentAdmission a;
                           a.id = "document-" + doc->summary().id;
                           a.format = doc->summary().format_id;
                           std::set<std::string> ids;
                           for (const auto &obj : doc->catalog().objects)
                               ids.insert(obj.locations.front().member_id);
                           for (const auto &member : sources.members)
                               if (ids.contains(member->id)) {
                                   a.member_ids.push_back(member->id);
                                   if (!a.primary)
                                       a.primary = member;
                                   else
                                       a.companion_sources.push_back(member->reference);
                               }
                           configure_companions(a, sources, typed);
                           a.native = std::make_unique<State>(std::move(typed));
                           return a;
                       });
}
} // namespace axk::app::formats::typhoon
