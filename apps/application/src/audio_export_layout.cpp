#include "audio_export_layout.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <ranges>
#include <string>
#include <string_view>

std::string axk::app::safe_audio_export_path_name(std::string_view value, std::string_view fallback) {
    auto text = std::string{value};
    const auto first = text.find_first_not_of(" \t\r\n");
    const auto last = text.find_last_not_of(" \t\r\n");
    text = first == std::string::npos ? std::string{fallback} : text.substr(first, last - first + 1U);
    std::size_t stars{};
    while (!text.empty() && text.back() == '*') {
        ++stars;
        text.pop_back();
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
        text.pop_back();
    std::string result;
    bool prior_space{};
    bool prior_underscore{};
    for (const auto character : text) {
        if (character == '<') {
            result += "_lt_";
            prior_underscore = true;
            prior_space = false;
        } else if (character == '>') {
            result += "_gt_";
            prior_underscore = true;
            prior_space = false;
        } else if (std::string_view{"\\/:*?\"|"}.contains(character) || static_cast<unsigned char>(character) < 0x20U) {
            if (!prior_underscore)
                result.push_back('_');
            prior_underscore = true;
            prior_space = false;
        } else if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            if (!result.empty() && !prior_space)
                result.push_back(' ');
            prior_space = true;
            prior_underscore = false;
        } else {
            result.push_back(character);
            prior_space = false;
            prior_underscore = character == '_';
        }
    }
    while (!result.empty() && (result.back() == ' ' || result.back() == '.' || result.back() == '_'))
        result.pop_back();
    while (!result.empty() && (result.front() == ' ' || result.front() == '.' || result.front() == '_'))
        result.erase(result.begin());
    if (result.empty())
        result = fallback;
    if (stars != 0U)
        result += std::format(" ({})", stars + 1U);
    return result;
}

axk::Result<void> axk::app::apply_audio_export_layout(ExportPlan &plan, const AudioExportLayout &layout,
                                                      PooledPathAllocator &pooled_paths) {
    for (auto &volume : plan.volumes) {
        volume.relative_root =
            layout.preserve_volume_roots ? layout.selection_root / volume.relative_root : layout.selection_root;
        std::map<std::string, std::filesystem::path> waveform_paths;
        for (auto &waveform : volume.waveforms) {
            auto pooled = pooled_paths.allocate(volume.relative_root, "physical",
                                                safe_audio_export_path_name(waveform.display_name, "sample"),
                                                audio_internal::WavSource::from_physical(waveform.waveform));
            if (!pooled)
                return std::unexpected{pooled.error()};
            waveform.relative_wav_path = std::move(*pooled);
            waveform_paths.emplace(waveform.object_key, waveform.relative_wav_path);
        }
        for (auto &sample : volume.samples) {
            for (auto &member : sample.members) {
                if (const auto found = waveform_paths.find(member.waveform_key); found != waveform_paths.end())
                    member.relative_wav_path = found->second;
            }
            if (!layout.render_stereo) {
                sample.rendered_wav_path.reset();
                continue;
            }
            if (sample.rendered_wav_path && sample.members.size() == 2U) {
                const auto left = std::ranges::find(volume.waveforms, sample.members[0].waveform_key,
                                                    &PhysicalWaveformExport::object_key);
                const auto right = std::ranges::find(volume.waveforms, sample.members[1].waveform_key,
                                                     &PhysicalWaveformExport::object_key);
                if (left != volume.waveforms.end() && right != volume.waveforms.end()) {
                    auto pooled = pooled_paths.allocate(
                        volume.relative_root, "rendered", safe_audio_export_path_name(sample.display_name, "sample"),
                        audio_internal::WavSource::from_stereo(left->waveform, right->waveform));
                    if (!pooled)
                        return std::unexpected{pooled.error()};
                    sample.rendered_wav_path = std::move(*pooled);
                }
            }
        }
    }
    for (auto &scope : plan.unresolved_wave_data) {
        scope.relative_root = layout.selection_root / scope.relative_root;
        for (auto &waveform : scope.waveforms) {
            auto pooled = pooled_paths.allocate(scope.relative_root, "physical",
                                                safe_audio_export_path_name(waveform.display_name, "wave-data"),
                                                audio_internal::WavSource::from_physical(waveform.waveform));
            if (!pooled)
                return std::unexpected{pooled.error()};
            waveform.relative_wav_path = std::move(*pooled);
        }
    }
    return {};
}
