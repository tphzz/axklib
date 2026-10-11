#include "scsi2sd_common.hpp"

#include <format>
#include <map>
#include <tuple>

namespace rawdisk::scsi2sd {
bool same_target_layout(std::span<const Target> first, std::span<const Target> second) {
    using Layout = std::map<std::uint8_t, std::tuple<std::uint8_t, std::uint32_t, std::uint32_t, std::uint16_t>>;
    const auto collect = [](std::span<const Target> targets, Layout &result) {
        for (const auto &target : targets) {
            if (target.enabled &&
                !result
                     .emplace(target.scsi_id, std::tuple{target.device_type, target.sd_sector_start,
                                                         target.scsi_sectors, target.bytes_per_sector})
                     .second)
                return false;
        }
        return true;
    };
    Layout left;
    Layout right;
    return collect(first, left) && collect(second, right) && left == right;
}
} // namespace rawdisk::scsi2sd

namespace rawdisk::scsi2sd::detail {
namespace {

constexpr std::uint64_t kMisplacedOffset = 1024; // see v6::misplaced_save_lba

std::string label_of(const Target &t) {
    std::string name = t.vendor;
    if (!t.product.empty())
        name += (name.empty() ? "" : " ") + t.product;
    return name.empty() ? std::format("SCSI ID {}", t.scsi_id) : std::format("SCSI ID {}: {}", t.scsi_id, name);
}

void note(Volume &v, Severity s, const char *code, std::string text) {
    v.notices.push_back({s, code, std::format("SCSI ID {}: {}", v.id, text)});
}

} // namespace

VolumeSet build_volumes(std::string scheme, std::span<const Target> targets, std::uint64_t n, bool v6_board,
                        std::vector<Range> protected_ranges) {
    VolumeSet set;
    set.scheme = std::move(scheme);
    set.parent_sector_size = kCardSectorSize;
    set.parent_sectors = n;
    set.protected_ranges = std::move(protected_ranges);
    const Range config = v6_board && n >= 2 ? Range{n - 2, n} : Range{};
    const Range misplaced =
        v6_board && n >= kMisplacedOffset ? Range{n - kMisplacedOffset, n - kMisplacedOffset + 2} : Range{};

    for (const Target &t : targets) {
        if (!t.enabled)
            continue;
        Volume v;
        v.id = std::to_string(t.scsi_id);
        v.label = label_of(t);
        v.sector_size = t.bytes_per_sector;
        v.read_only = t.device_type != static_cast<std::uint8_t>(DeviceType::fixed);

        if (t.bytes_per_sector == 0 || t.scsi_sectors == 0) {
            v.usable = false;
            note(v, Severity::warning, "s2s.no_size", "enabled, but without a size or sector size.");
            set.volumes.push_back(std::move(v));
            continue;
        }
        if (t.bytes_per_sector < 64 || t.bytes_per_sector > 8192)
            note(v, Severity::warning, "s2s.sector_size_range",
                 std::format("sector size {} is outside the firmware's range (64..8192).", t.bytes_per_sector));
        const bool supported = t.bytes_per_sector % kCardSectorSize == 0;
        if (!supported) {
            v.usable = false;
            note(v, Severity::warning, "s2s.sector_size_unsupported",
                 std::format("sector size {} is not a multiple of 512; listed but not accessible "
                             "(the firmware's on-card packing is not verified).",
                             t.bytes_per_sector));
        }
        if (t.sd_sector_start >= n) {
            v.usable = false;
            note(v, Severity::warning, "s2s.beyond_card_end",
                 std::format("starts at card sector {}, beyond the card end ({} sectors).", t.sd_sector_start, n));
            v.range = {t.sd_sector_start, t.sd_sector_start};
            set.volumes.push_back(std::move(v));
            continue;
        }

        // Configured size, cut at the card end in whole SCSI sectors (as the firmware does).
        const std::uint64_t avail = n - t.sd_sector_start;
        std::uint64_t len = std::min(t.card_sectors(), avail);
        if (supported) {
            const std::uint64_t ratio = t.bytes_per_sector / kCardSectorSize;
            len = len / ratio * ratio;
            v.sector_count = len / ratio;
        }
        v.range = {t.sd_sector_start, t.sd_sector_start + len};
        if (len < t.card_sectors())
            note(v, Severity::warning, "s2s.truncated",
                 std::format("configured size exceeds the card; only {} of {} card sectors exist.", len,
                             t.card_sectors()));
        if (supported && v.sector_count == 0)
            v.usable = false;
        if (v.range.overlaps(config))
            note(v, Severity::danger, "s2s.covers_config",
                 "covers the last 2 card sectors, where V6 stores its configuration; the SCSI host can "
                 "overwrite the configuration through this disk.");
        if (v.range.overlaps(misplaced))
            note(v, Severity::warning, "s2s.covers_misplaced_save",
                 std::format("covers card sectors {}..{}, where V6 firmware 0x0632 writes its configuration by "
                             "mistake when a host changes the sector size (MODE SELECT).",
                             misplaced.begin, misplaced.end - 1));
        set.volumes.push_back(std::move(v));
    }

    for (std::size_t i = 0; i < set.volumes.size(); ++i)
        for (std::size_t j = i + 1; j < set.volumes.size(); ++j) {
            Volume &a = set.volumes[i];
            Volume &b = set.volumes[j];
            if (a.range.overlaps(b.range)) {
                note(a, Severity::danger, "s2s.overlap",
                     std::format("overlaps SCSI ID {}: writing one corrupts the other.", b.id));
                note(b, Severity::danger, "s2s.overlap",
                     std::format("overlaps SCSI ID {}: writing one corrupts the other.", a.id));
            }
            if (a.id == b.id) {
                for (Volume *v : {&a, &b}) {
                    if (!has_notice(v->notices, "s2s.duplicate_id"))
                        note(*v, Severity::danger, "s2s.duplicate_id", "this SCSI ID is enabled more than once.");
                    v->usable = false;
                }
            }
        }

    bool any = false;
    for (const Volume &v : set.volumes)
        any |= v.usable;
    if (!any)
        set.notices.push_back({Severity::warning, "s2s.no_volumes", "No usable virtual disk is enabled."});
    return set;
}

} // namespace rawdisk::scsi2sd::detail
