#include "axklib/conversion/tx16w/wave_disk_writer.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "axklib/filesystems/fat/floppy_writer.hpp"
#include "axklib/tx16w_wave_mapping.hpp"

namespace axk::conversion::tx16w_wave_disk {
namespace {
constexpr auto layout = filesystems::FloppyLayout::dd_720k;
constexpr std::size_t wave_slots = 64U;

Error invalid(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::audio, std::move(message));
}

enum class Media { single, set };
struct DiskSlice {
    std::size_t begin{}, end{};
    std::uint64_t clusters{};
};
std::string member_name(std::size_t number) {
    return std::string(number < 10U ? "DISK0" : "DISK") + std::to_string(number) + ".IMG";
}

Result<std::shared_ptr<const PreparedPlan>> prepare(Media media, std::string_view profile_id,
                                                    std::span<const WaveTransfer> waves, const Request &request,
                                                    const CancellationToken &cancel, ProgressSink *) {
    const auto capacity = filesystems::floppy_capacity(layout);
    if (!capacity)
        return std::unexpected(capacity.error());
    Report report{std::string(profile_id), request.fidelity, true, {}, {}, NewImageReview{}};
    const auto issue = [&](const Action &a, std::string code, Impact impact, std::string message) {
        auto id = std::to_string(report.issues.size() + 1U) + "-" + code;
        report.issues.push_back(
            {std::move(id), "tx16w." + code, impact, a.source_object, code, a.name, code, std::move(message)});
        if (impact == Impact::blocker || (request.fidelity == Fidelity::strict && impact == Impact::loss))
            report.executable = false;
    };
    std::vector<tx16w::WaveEncoding> encodings;
    std::vector<DiskSlice> slices(1);
    if (waves.size() > wave_slots) {
        issue({}, "wave-slots", Impact::blocker,
              "Selected " + std::to_string(waves.size()) + " Waves; the disk profile has 64 Wave slots");
    } else {
        for (std::size_t i = 0; i < waves.size(); ++i) {
            const auto &wave = waves[i];
            auto &action =
                report.actions.emplace_back(Action{"wave", wave_filename(wave.name, i + 1U), wave.id, "create"});
            action.details = {{"Source name", wave.name}, {"Wave slot", std::to_string(i + 1U)}};
            auto mapped = map_playback(wave);
            if (!mapped) {
                issue(action, "playback", Impact::blocker, mapped.error().message);
                continue;
            }
            encodings.push_back(*mapped);
            const auto frames = mapped->attack_frames + mapped->repeat_frames;
            const auto size = tx16w::wave_file_size(frames);
            const auto used = (size.padded_bytes + capacity->cluster_bytes - 1U) / capacity->cluster_bytes;
            if (media == Media::set && slices.back().clusters > 0 &&
                slices.back().clusters + used > capacity->data_clusters)
                slices.push_back({i, i, 0});
            auto &slice = slices.back();
            slice.clusters += used;
            slice.end = i + 1U;
            if (media == Media::set) {
                action.details.push_back({"Disk", member_name(slices.size())});
                if (used > capacity->data_clusters)
                    issue(action, "disk-space", Impact::blocker, "This Wave cannot fit intact on an empty floppy");
            }
            for (auto [label, value] : {std::pair{"Sample rate", mapped->sample_rate},
                                        {"Frames", frames},
                                        {"Attack frames", mapped->attack_frames},
                                        {"Repeat frames", mapped->repeat_frames}})
                action.details.push_back({label, std::to_string(value)});
            action.details.push_back({"Native bytes", std::to_string(size.native_bytes)});
            action.details.push_back({"Wave file bytes", std::to_string(size.padded_bytes)});
            action.details.push_back({"Data clusters", std::to_string(used)});
            action.details.push_back({"Playback", mapped->looped ? "Forward loop" : "One-shot"});
            action.details.push_back({"Output precision", "12-bit signed PCM"});
            if (mapped->sample_rate != wave.rate)
                issue(action, "sample-rate", Impact::loss,
                      "Resample " + std::to_string(wave.rate) + " Hz to " + std::to_string(mapped->sample_rate) +
                          " Hz");
            if (mapped->looped &&
                (static_cast<std::uint64_t>(wave.loop_start - wave.first_frame) * mapped->sample_rate % wave.rate !=
                     0U ||
                 static_cast<std::uint64_t>(wave.frame_count) * mapped->sample_rate % wave.rate != 0U))
                issue(action, "loop-rounding", Impact::loss,
                      "Loop boundaries are rounded to the nearest destination frame");
            if (action.name.substr(0, action.name.size() - 4U) != wave.name)
                issue(action, "wave-name", Impact::representation,
                      "Map source name " + wave.name + " to " + action.name);
            issue(action, "pitch-metadata", Impact::loss,
                  "A Wave-only disk does not store the source root key " + std::to_string(wave.root_key) +
                      " or fine tune (" + std::to_string(wave.fine_tune) + " cents)");
        }
    }
    const auto clusters = slices.front().clusters;
    if (media == Media::single && clusters > capacity->data_clusters)
        issue({}, "disk-space", Impact::blocker,
              "Selected Waves require " + std::to_string(clusters) + " data clusters; " +
                  std::to_string(capacity->data_clusters) + " available");
    if (media == Media::single && !report.actions.empty()) {
        report.actions.back().details.push_back(
            {"Disk allocation", std::to_string(clusters) + " / " + std::to_string(capacity->data_clusters) +
                                    " data clusters; " + std::to_string(waves.size()) + " / " +
                                    std::to_string(wave_slots) + " Wave slots"});
    }

    if (media == Media::set) {
        for (const auto &slice : slices)
            if (slice.end > slice.begin)
                report.actions[slice.end - 1U].details.push_back(
                    {"Disk allocation", std::to_string(slice.clusters) + " / " +
                                            std::to_string(capacity->data_clusters) + " data clusters"});
    }

    NewImage single;
    NewImageSet set;
    std::uint64_t output_bytes{};
    if (report.executable) {
        for (const auto &slice : slices) {
            std::vector<std::vector<std::byte>> payloads;
            std::vector<filesystems::FloppyFile> files;
            for (std::size_t i = slice.begin; i < slice.end; ++i) {
                if (auto active = cancel.check(); !active)
                    return std::unexpected(active.error());
                const auto &wave = waves[i];
                const auto &encoding = encodings[i];
                const auto frames = encoding.attack_frames + encoding.repeat_frames;
                const auto working = output_bytes + 2U * capacity->disk_bytes + report_bytes(report) +
                                     (static_cast<std::uint64_t>(wave.frame_count) + frames + 32U) * 16U;
                if (working > request.limits.maximum_bytes)
                    return std::unexpected(invalid("Wave conversion exceeds its working memory limit"));
                auto rendered = render_wave(wave, encoding, cancel);
                if (!rendered)
                    return std::unexpected(rendered.error());
                if (rendered->rounded)
                    issue(report.actions[i], "precision", Impact::loss,
                          "Round audio to signed 12-bit precision, saturating peaks outside the native range");
                payloads.push_back(std::move(rendered->bytes));
                files.push_back({report.actions[i].name, std::span<const std::byte>{payloads.back()}});
            }
            if (report.executable) {
                auto disk = filesystems::build_floppy(layout, files, cancel);
                if (!disk)
                    return std::unexpected(disk.error());
                output_bytes += disk->capacity();
                if (media == Media::single)
                    single.bytes = std::move(*disk);
                else
                    set.members.push_back({member_name(set.members.size() + 1U), std::move(*disk)});
            }
        }
    }
    if (media == Media::set) {
        if (!report.executable) {
            set = {};
            output_bytes = 0;
        }
        return std::make_shared<const PreparedPlan>(std::move(report), std::move(set));
    }
    const auto retained = report_bytes(report) + single.bytes.capacity();
    return std::make_shared<const PreparedPlan>(std::move(report), std::move(single), retained);
}
} // namespace

WaveWriterAdapter writer() {
    return {"tx16w.wave-disk",
            "yamaha.tx16w",
            "a TX16W Wave disk",
            OutputKind::new_image,
            RouteDescriptor{"TX16W · Yamaha Wave disk", "fat12-720k", {"WAVE_DATA"}, {"new-image-file"}, 0},
            [](auto &&...args) { return prepare(Media::single, args...); }};
}

WaveWriterAdapter set_writer() {
    return {"tx16w.wave-disk-set",
            "yamaha.tx16w",
            "a TX16W Wave disk set",
            OutputKind::new_image_set,
            RouteDescriptor{"Yamaha TX16W - Wave floppy set", "fat12-720k-set", {"WAVE_DATA"}, {"new-image-set"}, 0},
            [](auto &&...args) { return prepare(Media::set, args...); }};
}
} // namespace axk::conversion::tx16w_wave_disk
