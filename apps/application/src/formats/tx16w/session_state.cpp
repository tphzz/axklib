#include "formats/tx16w/session_state.hpp"

#include <algorithm>
#include <type_traits>
#include <unordered_map>
#include <variant>

namespace axk::app::formats::tx16w {

SessionState::SessionState(std::shared_ptr<const axk::tx16w::Document> document) : document_(std::move(document)) {
    native_document = document_;
    index_native_catalog();
}

Result<nlohmann::ordered_json> SessionState::object_detail(const ImageDocumentContext &context,
                                                           std::string_view object_id) const {
    auto object = document_->object(object_id);
    if (!object)
        return std::unexpected(Error{"object_not_found", "TX16W object does not exist"});
    using Json = nlohmann::ordered_json;
    const auto &value = **object;
    Json details = std::visit(
        [](const auto &item) -> Json {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, axk::tx16w::WaveHeader>) {
                return {
                    {"type", "wave"},
                    {"sampleRate", item.sample_rate},
                    {"sampleRateApproximate", item.sample_rate_source == axk::tx16w::SampleRateSource::defaulted_33333},
                    {"nativeBitDepth", 12},
                    {"attackFrames", item.attack_frames},
                    {"repeatFrames", item.repeat_frames},
                    {"looped", item.looped}};
            } else if constexpr (std::is_same_v<T, axk::tx16w::Voice>) {
                auto regions = Json::array();
                for (const auto &region : item.regions)
                    regions.push_back({{"timbreSlot", region.timbre_slot},
                                       {"lowKey", region.low_key_number},
                                       {"highKey", region.high_key_number},
                                       {"fade", region.fade}});
                return {{"type", "voice"}, {"slot", item.slot}, {"regions", std::move(regions)}};
            } else if constexpr (std::is_same_v<T, axk::tx16w::Timbre>) {
                return {{"type", "timbre"},
                        {"slot", item.slot},
                        {"waveSlot", item.wave_slot},
                        {"rootKey", item.root_key_number}};
            } else if constexpr (std::is_same_v<T, axk::tx16w::Performance>) {
                auto voices = Json::array();
                for (const auto &voice : item.voices)
                    voices.push_back({{"receiveChannel", voice.receive_channel},
                                      {"voiceSlot", voice.voice_slot},
                                      {"alternativeGroup", voice.alternative_group},
                                      {"audioOutput", voice.audio_output},
                                      {"volume", voice.volume},
                                      {"detune", voice.detune},
                                      {"transpose", voice.transpose}});
                return {{"type", "performance"}, {"slot", item.slot}, {"voices", std::move(voices)}};
            } else if constexpr (std::is_same_v<T, axk::tx16w::NativeSetup>) {
                auto waves = Json::array();
                for (const auto &wave : item.waves)
                    waves.push_back({{"slot", wave.slot}, {"name", wave.name}});
                return {{"type", "setup"}, {"writeProtected", item.write_protected}, {"waves", std::move(waves)}};
            } else {
                return {{"type", "opaque"}, {"kind", std::string(1U, item.kind)}};
            }
        },
        value.value);
    const auto &summary = objects.at(object_indices_by_id.at(std::string{object_id}));
    return Json{{"imageId", context.image_id},
                {"documentId", context.document_id},
                {"revision", context.revision},
                {"formatId", "yamaha.tx16w"},
                {"profile", "yamaha-native"},
                {"objectId", value.id},
                {"type", summary.type},
                {"name", summary.name},
                {"details", std::move(details)},
                {"locations", Json::array({{{"memberId", value.location.member_id},
                                            {"rootId", value.location.root_id},
                                            {"entryId", value.location.entry_id},
                                            {"offset", value.location.offset},
                                            {"length", value.location.length}}})}};
}

Result<void> SessionState::preserve_identity(const NativeDocumentState &previous) {
    if (previous.format_id() != format_id())
        return std::unexpected(Error{"native_format_changed", "Native format changed during refresh"});
    // Engine identities are occurrence-based and stable for the explicit source set.
    return {};
}

} // namespace axk::app::formats::tx16w
