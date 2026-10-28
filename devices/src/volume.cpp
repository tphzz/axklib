#include "rawdisk/volume.hpp"
#include "detail.hpp"

#include <algorithm>

namespace rawdisk {
namespace {

using detail::fail;

// The unique volume with this id; layout_invalid if ambiguous, invalid_argument if absent.
Result<const Volume *> lookup(const VolumeSet &set, std::string_view id) {
    const Volume *found = nullptr;
    for (const Volume &v : set.volumes) {
        if (v.id != id)
            continue;
        if (found)
            return fail(Errc::layout_invalid);
        found = &v;
    }
    if (!found)
        return fail(Errc::invalid_argument);
    return found;
}

Result<void> writable_impl(const VolumeSet &set, const Volume &v, const VolumeOpenOptions &options) {
    if (v.read_only)
        return fail(Errc::read_only_handle);
    if (options.allow_overlap)
        return {};
    for (const Volume &o : set.volumes)
        if (&o != &v && o.range.overlaps(v.range))
            return fail(Errc::layout_conflict);
    return {};
}

} // namespace

std::string_view to_string(Severity s) noexcept {
    switch (s) {
    case Severity::info:
        return "info";
    case Severity::warning:
        return "warning";
    case Severity::danger:
        return "danger";
    }
    return "unknown";
}

bool has_notice(const std::vector<Notice> &notices, std::string_view code) noexcept {
    return std::ranges::any_of(notices, [&](const Notice &n) { return n.code == code; });
}

bool has_severity(const std::vector<Notice> &notices, Severity at_least) noexcept {
    return std::ranges::any_of(notices, [&](const Notice &n) { return n.severity >= at_least; });
}

const Volume *VolumeSet::find(std::string_view id) const noexcept {
    const Volume *found = nullptr;
    for (const Volume &v : volumes) {
        if (v.id != id)
            continue;
        if (found)
            return nullptr;
        found = &v;
    }
    return found;
}

Result<void> check_writable(const VolumeSet &set, std::string_view id, const VolumeOpenOptions &options) noexcept {
    return detail::guard([&]() -> Result<void> {
        auto v = lookup(set, id);
        if (!v)
            return fail(v.error());
        if (!(*v)->usable)
            return fail(Errc::layout_invalid);
        return writable_impl(set, **v, options);
    });
}

Result<std::shared_ptr<BlockDevice>> open_volume(std::shared_ptr<BlockDevice> parent, const VolumeSet &set,
                                                 std::string_view id, Access access,
                                                 const VolumeOpenOptions &options) noexcept {
    return detail::guard([&]() -> Result<std::shared_ptr<BlockDevice>> {
        if (!parent)
            return fail(Errc::invalid_argument);
        if (parent->sector_size() != set.parent_sector_size || parent->sector_count() != set.parent_sectors)
            return fail(Errc::device_changed); // the set describes a different device
        auto v = lookup(set, id);
        if (!v)
            return fail(v.error());
        const Volume &vol = **v;
        if (!vol.usable || vol.range.empty() || vol.sector_count == 0)
            return fail(Errc::layout_invalid);
        const bool rw = access != Access::ReadOnly;
        if (rw) {
            if (!parent->writable())
                return fail(Errc::read_only_handle);
            if (auto w = writable_impl(set, vol, options); !w)
                return fail(w.error());
        }
        SliceOptions so;
        so.writable = rw;
        if (options.protect)
            so.protected_ranges = set.protected_ranges;
        return make_slice(std::move(parent), vol.range.begin, vol.sector_count, vol.sector_size, std::move(so));
    });
}

} // namespace rawdisk
