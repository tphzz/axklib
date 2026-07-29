#include "axklib/audio_export.hpp"

#include "audio_export_support.hpp"
#include "axklib/export_paths.hpp"
#include "axklib/file_publication.hpp"
#include "axklib/utf8.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace axk {
namespace {

using audio_export_detail::append_publication_warnings;
using audio_export_detail::safe_component;

std::string display_text(std::string value, std::string_view fallback) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0)
        value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0)
        value.pop_back();
    return value.empty() ? std::string{fallback} : value;
}

struct SamplePlaybackWindow {
    std::uint64_t start{};
    std::uint64_t end{};
    std::optional<std::uint64_t> loop_start;
    std::optional<std::uint64_t> loop_end;
    bool one_shot{};
};

const CurrentSbnkMember *sample_member(const SampleExport &sample, std::string_view role) {
    const auto *member = &sample.decoded.left;
    if (role == "right" && sample.decoded.right)
        member = &*sample.decoded.right;
    return member;
}

Result<SamplePlaybackWindow> sample_playback_window(const SampleExport &sample, const PhysicalWaveformExport &waveform,
                                                    std::string_view role) {
    const auto *member = sample_member(sample, role);
    if (role == "right" && !sample.decoded.right) {
        return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::audio,
                                          "SFZ Sample right member metadata is missing")};
    }
    if (member->wave_length_frames == 0U) {
        return std::unexpected{
            make_error(ErrorCode::object_malformed, ErrorCategory::audio, "SFZ Sample playback window is empty")};
    }
    const auto start = static_cast<std::uint64_t>(member->wave_start_frame);
    const auto length = static_cast<std::uint64_t>(member->wave_length_frames);
    if (start > waveform.waveform.frame_count || length > waveform.waveform.frame_count - start) {
        return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::audio,
                                          "SFZ Sample playback window exceeds its confirmed Wave Data")};
    }
    SamplePlaybackWindow result;
    result.start = start;
    result.end = start + length - 1U;
    if (sample.decoded.loop_mode == 0U || sample.decoded.loop_mode == 4U) {
        result.one_shot = true;
        return result;
    }
    if (sample.decoded.loop_mode != 1U) {
        return std::unexpected{
            make_error(ErrorCode::audio_unsupported_format, ErrorCategory::audio,
                       "SFZ exact export does not support reverse or bidirectional Sample playback modes")};
    }
    const auto loop_start = static_cast<std::uint64_t>(member->loop_start_frame);
    const auto loop_length = static_cast<std::uint64_t>(member->loop_length_frames);
    if (loop_length == 0U || loop_start < result.start || loop_start > result.end ||
        loop_length > result.end - loop_start + 1U) {
        return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::audio,
                                          "SFZ Sample loop lies outside its playback window")};
    }
    result.loop_start = loop_start;
    result.loop_end = loop_start + loop_length - 1U;
    return result;
}

bool rendered_window_compatible(const SampleExport &sample) {
    if (!sample.rendered_wav_path || sample.members.size() != 2U || !sample.decoded.right)
        return false;
    const auto &left = sample.decoded.left;
    const auto &right = *sample.decoded.right;
    return left.wave_start_frame == right.wave_start_frame && left.wave_length_frames == right.wave_length_frames &&
           left.loop_start_frame == right.loop_start_frame && left.loop_length_frames == right.loop_length_frames;
}

Result<std::string> sfz_region(const SampleExport &sample, const PhysicalWaveformExport &waveform,
                               std::string_view role, std::string sample_path, std::optional<int> pan) {
    const auto *member = sample_member(sample, role);
    const auto window = sample_playback_window(sample, waveform, role);
    if (!window)
        return std::unexpected{window.error()};
    std::string line{"<region>"};
    const auto key_low = sample.key_low == 255U ? member->root_key : sample.key_low;
    const auto key_high = sample.key_high == 128U ? member->root_key : sample.key_high;
    line += std::format(" lokey={} hikey={}", key_low, key_high);
    line += std::format(" pitch_keycenter={}", member->root_key);
    if (sample.coarse_tune >= -64 && sample.coarse_tune <= 64)
        line += std::format(" transpose={}", sample.coarse_tune);
    line += std::format(" tune={}", member->fine_tune_cents);
    if (pan)
        line += std::format(" pan={}", *pan);
    line += std::format(" offset={} end={}", window->start, window->end);
    if (window->one_shot) {
        line += " loop_mode=one_shot";
    } else {
        line +=
            std::format(" loop_mode=loop_continuous loop_start={} loop_end={}", *window->loop_start, *window->loop_end);
    }
    line += " sample=" + std::move(sample_path);
    return line;
}

Result<PublicationOutcome> write_text_atomic(const std::filesystem::path &path, std::string_view text, bool overwrite) {
    if (!overwrite && std::filesystem::exists(path)) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "refusing to replace an existing SFZ")};
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not create SFZ output directory")};
    }
    auto temporary = detail::TemporaryPublication::create(path, [&](const detail::TemporaryFileSink &sink) {
        return sink(std::as_bytes(std::span{text.data(), text.size()}));
    });
    if (!temporary)
        return std::unexpected{temporary.error()};
    const auto mode = overwrite ? detail::PublicationMode::replace_existing : detail::PublicationMode::create_only;
    auto published = temporary->publish(mode);
    if (!published)
        return std::unexpected{published.error()};
    return std::move(*published);
}

} // namespace

Result<SfzExportResult> write_sfz(const ExportPlan &plan, const std::filesystem::path &output_directory, bool overwrite,
                                  const CancellationToken &cancellation) {
    if (auto valid = audio_internal::validate_export_plan_paths(plan, output_directory); !valid)
        return std::unexpected{valid.error()};
    for (const auto &volume : plan.volumes) {
        for (const auto &sample : volume.samples) {
            for (const auto &member : sample.members) {
                if (member.quality != RelationshipQuality::known)
                    continue;
                const auto waveform =
                    std::ranges::find(volume.waveforms, member.waveform_key, &PhysicalWaveformExport::object_key);
                if (waveform == volume.waveforms.end()) {
                    return std::unexpected{make_error(ErrorCode::object_missing, ErrorCategory::audio,
                                                      "SFZ Sample references missing confirmed Wave Data")};
                }
                if (auto window = sample_playback_window(sample, *waveform, member.role); !window)
                    return std::unexpected{window.error()};
            }
        }
    }
    SfzExportResult result;
    std::vector<std::filesystem::path> planned_paths;
    std::set<std::filesystem::path> reserved_paths;
    const auto reserve_path = [&](const VolumeExport &volume, std::string name) {
        const std::array parts{volume.relative_root};
        const auto directory = *audio_internal::resolve_export_destination(output_directory, parts);
        const auto stem = safe_component(std::move(name), "instrument");
        auto path = directory / (stem + ".sfz");
        for (std::size_t index = 2U; reserved_paths.contains(path); ++index)
            path = directory / std::format("{} ({}).sfz", stem, index);
        reserved_paths.insert(path);
        planned_paths.push_back(std::move(path));
    };
    for (const auto &volume : plan.volumes) {
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        std::set<std::string> banked_samples;
        for (const auto &sample_bank : volume.sample_banks) {
            reserve_path(volume, "B " + display_text(sample_bank.display_name, "instrument"));
            banked_samples.insert(sample_bank.member_sample_keys.begin(), sample_bank.member_sample_keys.end());
        }
        for (const auto &sample : volume.samples) {
            if (!banked_samples.contains(sample.object_key))
                reserve_path(volume, sample.display_name);
        }
    }
    if (!overwrite) {
        const auto existing =
            std::ranges::find_if(planned_paths, [](const auto &path) { return std::filesystem::exists(path); });
        if (existing != planned_paths.end()) {
            return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                              "refusing to replace an existing SFZ: " + text::path_to_utf8(*existing))};
        }
    }
    auto planned_path = planned_paths.begin();
    for (const auto &volume : plan.volumes) {
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        std::set<std::string> banked_samples;
        const auto write_instrument = [&](std::string name,
                                          const std::vector<const SampleExport *> &samples) -> Result<void> {
            const auto path = *planned_path++;
            std::string text = std::format("// Generated by axklib\n// Volume: {}\n// "
                                           "Instrument: {}\n\n<group>\n",
                                           text::path_to_utf8(volume.relative_root), name);
            std::size_t region_count{};
            for (const auto *sample : samples) {
                if (const auto check = cancellation.check(); !check)
                    return std::unexpected{check.error()};
                if (rendered_window_compatible(*sample)) {
                    const auto waveform = std::ranges::find(volume.waveforms, sample->members.front().waveform_key,
                                                            &PhysicalWaveformExport::object_key);
                    if (waveform != volume.waveforms.end()) {
                        text += "// " + display_text(sample->display_name, "sample") + '\n';
                        auto region = sfz_region(*sample, *waveform, sample->members.front().role,
                                                 text::path_to_utf8(*sample->rendered_wav_path), {});
                        if (!region)
                            return std::unexpected{region.error()};
                        text += *region + '\n';
                        ++region_count;
                    }
                } else {
                    for (const auto &member : sample->members) {
                        if (member.quality != RelationshipQuality::known)
                            continue;
                        const auto waveform = std::ranges::find(volume.waveforms, member.waveform_key,
                                                                &PhysicalWaveformExport::object_key);
                        if (waveform == volume.waveforms.end())
                            continue;
                        const bool physical_pair =
                            sample->members.size() > 1U &&
                            std::ranges::any_of(sample->members,
                                                [](const auto &item) { return item.role == "left"; }) &&
                            std::ranges::any_of(sample->members, [](const auto &item) { return item.role == "right"; });
                        std::optional<int> pan;
                        if (physical_pair && member.role == "left")
                            pan = -100;
                        else if (physical_pair && member.role == "right")
                            pan = 100;
                        text += "// " + display_text(sample->display_name, "sample") + '\n';
                        auto region = sfz_region(*sample, *waveform, member.role,
                                                 text::path_to_utf8(member.relative_wav_path), pan);
                        if (!region)
                            return std::unexpected{region.error()};
                        text += *region + '\n';
                        ++region_count;
                    }
                }
            }
            if (text.empty() || text.back() != '\n')
                text += '\n';
            if (region_count == 0U)
                return {};
            const auto written = write_text_atomic(path, text, overwrite);
            if (!written)
                return std::unexpected{written.error()};
            append_publication_warnings(result.warnings, *written);
            result.written_files.push_back(path);
            return {};
        };
        for (const auto &sample_bank : volume.sample_banks) {
            std::vector<const SampleExport *> samples;
            for (const auto &key : sample_bank.member_sample_keys) {
                const auto found = std::ranges::find(volume.samples, key, &SampleExport::object_key);
                if (found != volume.samples.end()) {
                    samples.push_back(&*found);
                    banked_samples.insert(key);
                }
            }
            if (const auto written =
                    write_instrument("B " + display_text(sample_bank.display_name, "instrument"), samples);
                !written) {
                return std::unexpected{written.error()};
            }
        }
        for (const auto &sample : volume.samples) {
            if (banked_samples.contains(sample.object_key))
                continue;
            if (const auto written = write_instrument(sample.display_name, {&sample}); !written)
                return std::unexpected{written.error()};
        }
    }
    return result;
}

} // namespace axk
