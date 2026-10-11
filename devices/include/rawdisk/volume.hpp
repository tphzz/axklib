// Format-independent view of what a layout scheme (SCSI2SD V6, SCSI2SD XML, later partition
// tables) found on a device. Flows, UIs and parsers only ever work with this model.
#pragma once

#include "rawdisk/block_device.hpp"
#include "rawdisk/slice.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rawdisk {

enum class Severity { info, warning, danger };
std::string_view to_string(Severity s) noexcept;

// A finding worth showing. `code` is stable (documented in the README) so programs and UIs can
// react to it without parsing `text`.
struct Notice {
    Severity severity = Severity::info;
    std::string code{};
    std::string text{};
    friend bool operator==(const Notice &, const Notice &) = default;
};
bool has_notice(const std::vector<Notice> &notices, std::string_view code) noexcept;
bool has_severity(const std::vector<Notice> &notices, Severity at_least) noexcept;

struct Volume {
    std::string id{};               // stable within the scheme (SCSI2SD: the SCSI ID, "0".."7")
    std::string label{};            // human-readable
    Range range{};                  // parent sectors really occupied (cut at the device end)
    std::uint32_t sector_size = 0;  // the volume's own sector size
    std::uint64_t sector_count = 0; // in sector_size units
    bool usable = true;             // false: listed, but cannot be opened (see notices)
    std::vector<Notice> notices{};
    bool read_only = false;
};

struct VolumeSet {
    std::string scheme{};                 // "scsi2sd-v6", "scsi2sd-xml", ...
    std::uint32_t parent_sector_size = 0; // the set is only valid for a parent of this geometry
    std::uint64_t parent_sectors = 0;
    std::vector<Volume> volumes{};
    std::vector<Range> protected_ranges{}; // parent sectors no volume may write (metadata)
    std::vector<Notice> notices{};         // set-level findings

    // The volume with this id, or nullptr (also if the id is ambiguous).
    const Volume *find(std::string_view id) const noexcept;
};

struct VolumeOpenOptions {
    bool allow_overlap = false; // ReadWrite although the volume overlaps another one
    bool protect = true;        // enforce VolumeSet::protected_ranges
};

// Would open_volume(..., Access::ReadWrite, options) be allowed? Same checks, no I/O, so a tool
// can refuse before asking the user to confirm.
Result<void> check_writable(const VolumeSet &set, std::string_view id, const VolumeOpenOptions &options = {}) noexcept;

// Opens a volume as a slice of `parent`. Errc::device_changed if `parent` does not have the
// geometry the set was built for, Errc::invalid_argument for an unknown id, Errc::layout_invalid
// for an unusable or ambiguous one, Errc::layout_conflict for a refused overlap.
Result<std::shared_ptr<BlockDevice>> open_volume(std::shared_ptr<BlockDevice> parent, const VolumeSet &set,
                                                 std::string_view id, Access access,
                                                 const VolumeOpenOptions &options = {}) noexcept;

} // namespace rawdisk
