#include "axklib/conversion/a_series/wave_reader.hpp"

#include <algorithm>
#include <bit>
#include <unordered_map>
#include <utility>
#include <variant>

#include "axklib/audio.hpp"
#include "axklib/formats/a_series/native_document.hpp"
#include "axklib/sample_parameters.hpp"

namespace axk::conversion::a_series_waves {
namespace {
Error invalid(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::audio, std::move(message));
}
WavePlayback playback_mode(std::uint32_t mode) {
    switch (mode) {
    case static_cast<unsigned>(AudioSamplerLoopMode::forward):
        return WavePlayback::forward;
    case static_cast<unsigned>(AudioSamplerLoopMode::forward_loop):
        return WavePlayback::forward_loop;
    case static_cast<unsigned>(AudioSamplerLoopMode::forward_loop_release):
        return WavePlayback::forward_loop_release;
    case static_cast<unsigned>(AudioSamplerLoopMode::reverse):
        return WavePlayback::reverse;
    case static_cast<unsigned>(AudioSamplerLoopMode::forward_one_shot):
        return WavePlayback::forward_one_shot;
    case static_cast<unsigned>(AudioSamplerLoopMode::reverse_one_shot):
        return WavePlayback::reverse_one_shot;
    default:
        return WavePlayback::unknown;
    }
}
Result<std::vector<std::int16_t>> read_frames(const native::SourceSet &sources, native::SourceLocation range,
                                              std::uint16_t width, std::uint64_t first, std::size_t count,
                                              const CancellationToken &cancellation) {
    range.offset += first * width;
    range.length = count * width;
    auto raw = sources.read(range, cancellation);
    if (!raw)
        return std::unexpected(raw.error());
    std::vector<std::int16_t> pcm(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (width == 1U)
            pcm[i] = static_cast<std::int16_t>((std::to_integer<int>((*raw)[i]) - 128) * 256);
        else {
            const auto bits = static_cast<std::uint16_t>((std::to_integer<unsigned>((*raw)[2U * i]) << 8U) |
                                                         std::to_integer<unsigned>((*raw)[2U * i + 1U]));
            pcm[i] = std::bit_cast<std::int16_t>(bits);
        }
    }
    return pcm;
}
} // namespace

Result<std::vector<WaveTransfer>> select_waves(const Request &request, std::string_view document_id,
                                               std::span<const std::string> ids,
                                               const CancellationToken &cancellation) {
    const auto *doc = dynamic_cast<const formats::a_series::NativeDocument *>(request.source.get());
    if (!doc || document_id != doc->summary().id || ids.empty() || ids.size() > request.limits.maximum_actions)
        return std::unexpected(invalid("Select Wave Data from the current A-series document"));
    std::unordered_map<std::string, std::size_t> selected;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto *key = &ids[i];
        if (request.native_keys_by_id) {
            const auto it = request.native_keys_by_id->find(ids[i]);
            if (it == request.native_keys_by_id->end())
                return std::unexpected(invalid("Wave selection does not belong to this workspace document"));
            key = &it->second;
        }
        if (!selected.emplace(*key, i).second)
            return std::unexpected(invalid("Wave Data occurrence selected more than once"));
    }
    const auto &snapshots = doc->decoded()->state().inventory.catalog.objects;
    std::vector<WaveTransfer> result(ids.size());
    std::size_t found{};
    for (std::size_t i = 0; i < snapshots.size(); ++i) {
        if (auto active = cancellation.check(); !active)
            return std::unexpected(active.error());
        const auto &snapshot = snapshots[i];
        const auto &object = doc->catalog().objects[i];
        const auto it = selected.find(object.id);
        if (it == selected.end())
            continue;
        const auto *wave = std::get_if<CurrentSmpl>(&snapshot.object.payload);
        if (!wave || object.locations.size() != 1U)
            return std::unexpected(invalid("Select a complete, location-bound Wave Data occurrence"));
        if (auto supported = validate_smpl_pcm_transfer_control(*wave); !supported)
            return std::unexpected(supported.error());
        const auto width = wave->stored_sample_width_bytes.value;
        const auto start = wave->wave_start_frame.value;
        const auto frames = wave->wave_length_frames.value;
        auto location = object.locations.front();
        const auto pcm_size = wave->stored_pcm_bytes;
        if ((width != 1U && width != 2U) || wave->stored_segment_offset != 0U ||
            wave->stored_segment_bytes != pcm_size || pcm_size % width != 0U || wave->sample_rate.value == 0U ||
            wave->stored_pcm_offset > location.length || pcm_size > location.length - wave->stored_pcm_offset ||
            frames == 0U || start > pcm_size / width || frames > pcm_size / width - start)
            return std::unexpected(invalid("Incomplete or out-of-range Wave Data playback window"));
        location.offset += wave->stored_pcm_offset + static_cast<std::uint64_t>(start) * width;
        location.length = static_cast<std::uint64_t>(frames) * width;
        result[it->second] = {ids[it->second],
                              object.name,
                              wave->sample_rate.value,
                              start,
                              frames,
                              playback_mode(wave->loop_mode.value),
                              wave->loop_start_frame.value,
                              wave->loop_length_frames.value,
                              wave->root_key.value,
                              wave->fine_tune_cents.value,
                              [sources = doc->sources(), playback = std::move(location),
                               width](std::uint64_t first, std::size_t count, const CancellationToken &token) {
                                  return read_frames(*sources, playback, width, first, count, token);
                              }};
        ++found;
    }
    if (found != ids.size())
        return std::unexpected(invalid("Unknown Wave Data occurrence in selected document"));
    return result;
}

WaveReaderAdapter reader() {
    return {
        "yamaha.a-series",
        "A-series Wave Data",
        {{"type", "object"},
         {"required", {"documentId", "waveIds"}},
         {"additionalProperties", false},
         {"properties",
          {{"documentId", {{"type", "string"}}},
           {"waveIds", {{"type", "array"}, {"minItems", 1}, {"uniqueItems", true}, {"items", {{"type", "string"}}}}}}}},
        {"ordered-wave-selection"},
        [](const Request &request, const CancellationToken &cancel) -> Result<std::vector<WaveTransfer>> {
            const auto &options = request.options;
            if (!options.is_object() || options.size() != 2U || !options.contains("documentId") ||
                !options.at("documentId").is_string() || !options.contains("waveIds") ||
                !options.at("waveIds").is_array() || options.at("waveIds").size() > request.limits.maximum_actions ||
                !std::ranges::all_of(options.at("waveIds"), [](const auto &id) { return id.is_string(); }))
                return std::unexpected(invalid("Expected a document ID and an ordered list of Wave IDs"));
            return select_waves(request, options.at("documentId").get_ref<const std::string &>(),
                                options.at("waveIds").get<std::vector<std::string>>(), cancel);
        }};
}
} // namespace axk::conversion::a_series_waves
