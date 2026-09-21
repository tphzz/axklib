#include "a_series_sample_editor.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/application/sample_formats.hpp"
#include "axklib/audio.hpp"
#include "axklib/bytes.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/sample_parameter_json.hpp"
#include "axklib/writer_internal.hpp"

namespace axk::app::detail {
nlohmann::json a_series_sample_editor(const ObjectSnapshot &snapshot, std::span<const std::byte> bytes, bool writable,
                                      const nlohmann::json &sources) {
    const auto *sample = std::get_if<CurrentSbnk>(&snapshot.object.payload);
    if (!sample || !sample->storage.structurally_valid)
        return nullptr;
    const auto decoded = decode_sample_parameter_block(sample->raw_parameter_window,
                                                       *sample_parameter_generation(sample->storage.format));
    if (!decoded)
        return nullptr;
    const auto ordinary = sample->right ? (sample->sample_flags & 2U) == 0U &&
                                              sample->left.wave_data_name != sample->right->wave_data_name
                                        : (sample->sample_flags & 2U) != 0U;
    auto blocked = nlohmann::json::array();
    if (sample->right) {
        if (sample->left.root_key != sample->right->root_key)
            blocked.push_back("root_key");
        if (sample->left.fine_tune_cents != sample->right->fine_tune_cents)
            blocked.push_back("fine_tune_cents");
        if (sample->left.loop_start_frame != sample->right->loop_start_frame ||
            sample->left.loop_length_frames != sample->right->loop_length_frames) {
            blocked.push_back("loop_start_frame");
            blocked.push_back("loop_length_frames");
        }
        if ((sample->sample_flags & 6U) != 0U || sample->right->wave_data_name == sample->left.wave_data_name) {
            blocked.push_back("expand_detune");
            blocked.push_back("expand_dephase");
        }
    }
    std::uint64_t frames = maximum_wave_data_frames_per_channel;
    for (const auto &source : sources)
        frames = std::min(frames, source.at("frames").get<std::uint64_t>());
    if (sources.empty())
        frames = 0;
    const auto equal_windows =
        !sample->right || (sample->left.wave_start_frame == sample->right->wave_start_frame &&
                           sample->left.wave_length_frames == sample->right->wave_length_frames &&
                           sample->left.sample_rate == sample->right->sample_rate);
    const auto editable = writable && ordinary && snapshot.placement.has_value() && !sources.empty();
    std::vector<std::string> missing;
    const auto parameters = axk::detail::sample_parameters_json(decoded->parameters, &missing);
    const auto capabilities = sample_parameter_capabilities(*sample);
    auto unavailable = nlohmann::json::object();
    for (const auto &key : missing) {
        const auto &capability = capabilities.at(key);
        unavailable[key] = {
            {"reason", capability.at("available").get<bool>() ? "UNSUPPORTED_VALUE" : "FORMAT_UNAVAILABLE"},
            {"message", capability.at("reason")}};
    }
    auto blocked_reasons = nlohmann::json::object();
    for (const auto &key : blocked) {
        blocked_reasons[key.get<std::string>()] =
            key == "expand_detune" || key == "expand_dephase"
                ? "This retained expanded or duplicate-source layout needs a verified topology update. Its expansion "
                  "values are preserved."
                : "The stereo channels store different values. A shared edit cannot preserve both channel settings.";
    }
    return {{"profile", "a-series/sample"},
            {"editable", editable},
            {"reason", editable ? "" : "This Sample's layout, sources or image do not support editing"},
            {"payloadSha256", package_internal::hex_digest(package_internal::sha256(bytes))},
            {"partitionIndex", snapshot.partition.value},
            {"volumeName", snapshot.placement ? snapshot.placement->volume_name : ""},
            {"parameters", parameters},
            {"unavailableParameters", unavailable},
            {"eqCoefficients", decoded->eq_coefficients},
            {"blockedParameters", blocked},
            {"blockedParameterReasons", blocked_reasons},
            {"sampleFormat", sample_format_metadata(*sample)},
            {"parameterCapabilities", capabilities},
            {"playbackWindow",
             {{"start_frame", sample->left.wave_start_frame}, {"length_frames", sample->left.wave_length_frames}}},
            {"canEditPlayback", editable && equal_windows},
            {"maximumFrames", frames},
            {"sources", sources}};
}
} // namespace axk::app::detail
