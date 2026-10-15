#include "formats/tx16w/session_state.hpp"

#include <algorithm>
#include <type_traits>
#include <unordered_map>
#include <variant>

namespace axk::app::formats::tx16w {

SessionState::SessionState(std::shared_ptr<const axk::tx16w::Document> document) : document_(std::move(document)) {
    native_document = document_;
    const auto &catalog = document_->catalog();
    content_children.emplace("", std::vector<std::size_t>{});
    std::unordered_map<std::string, std::size_t> nodes;
    for (const auto &node : catalog.nodes) {
        ImageContentItem item{};
        item.id = node.id;
        item.parent_id = node.parent_id;
        item.object_id = node.object_id;
        item.kind = node.type;
        item.name = item.display_name = node.name;
        item.scope_role = node.type;
        item.quality = "native";
        nodes.emplace(item.id, content.size());
        content_children[item.parent_id.value_or("")].push_back(content.size());
        content_children.try_emplace(item.id);
        content.push_back(std::move(item));
    }
    for (auto &node : content) {
        node.child_count = content_children.at(node.id).size();
        auto parent = node.parent_id;
        while (parent) {
            ++node.depth;
            parent = content.at(nodes.at(*parent)).parent_id;
        }
    }
    root_count = content_children.at("").size();
    for (const auto &node : content)
        object_indices_by_content_scope.try_emplace(node.id);
    for (const auto &object : catalog.objects) {
        ImageObjectItem item{};
        item.id = object.id;
        if (object.type != "native-file")
            native_object_ids.emplace(object.id, object.id);
        item.type = object.type;
        item.name = object.name;
        item.format = "yamaha.tx16w";
        for (const auto &location : object.locations)
            item.stored_size_bytes += location.length;
        object_indices_by_id.emplace(item.id, objects.size());
        object_indices_by_type[item.type].push_back(objects.size());
        for (const auto &node : content) {
            if (node.object_id != item.id)
                continue;
            auto scope = std::optional{node.id};
            while (scope) {
                object_indices_by_content_scope[*scope].push_back(objects.size());
                scope = content.at(nodes.at(*scope)).parent_id;
            }
        }
        objects.push_back(std::move(item));
    }
    for (const auto &edge : catalog.relationships) {
        ImageRelationshipItem item{};
        item.id = edge.id;
        item.source_object_id = edge.source_id;
        item.target_object_id = edge.target_id;
        item.candidate_object_ids = edge.candidates;
        item.type = edge.type;
        item.quality = edge.resolution == native::Resolution::resolved    ? "resolved"
                       : edge.resolution == native::Resolution::ambiguous ? "ambiguous"
                                                                          : "missing";
        item.basis = edge.native_reference;
        relationships.push_back(std::move(item));
    }
    for (const auto &issue : catalog.diagnostics)
        validation.push_back({issue.code,
                              issue.severity == native::Severity::error     ? "error"
                              : issue.severity == native::Severity::warning ? "warning"
                                                                            : "info",
                              issue.message,
                              {},
                              issue.object_id});
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
