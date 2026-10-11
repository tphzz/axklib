#include "detail.hpp"
#include "rawdisk/open/blocks.hpp"
#include "rawdisk/open/modes.hpp"

namespace rawdisk::open {
namespace {

template <class... F> std::vector<std::unique_ptr<Block>> blocks(F &&...f) {
    std::vector<std::unique_ptr<Block>> v;
    (v.push_back(f()), ...);
    return v;
}

} // namespace

Result<void> Registry::add(std::shared_ptr<const Mode> mode) {
    if (!mode || mode->info().id.empty() || find(mode->info().id))
        return detail::fail(Errc::invalid_argument);
    modes_.push_back(std::move(mode));
    return {};
}

std::shared_ptr<const Mode> Registry::find(std::string_view id) const noexcept {
    for (const auto &m : modes_)
        if (m->info().id == id)
            return m;
    return nullptr;
}

Registry builtin_modes(SelectionLimits limits) {
    using scsi2sd::xml::Board;
    Registry r;
    (void)r.add(make_mode({"image", "Image file", "A raw sector image file."},
                          [] { return blocks([] { return select_image(); }); }));
    (void)r.add(make_mode({"raw", "Raw device", "A whole USB disk, sector by sector."},
                          [limits] { return blocks([limits] { return select_device(limits.max_device_bytes); }); }));
    (void)r.add(make_mode(
        {"scsi2sd-v6", "SCSI2SD V6 card", "A virtual disk on a SCSI2SD V6 card; the layout is read from the card."},
        [limits] {
            return blocks([] { return select_device(); }, scsi2sd_v6,
                          [limits] { return select_volume(limits.max_volume_bytes); });
        }));
    (void)r.add(make_mode({"scsi2sd-v5", "SCSI2SD V5 card",
                           "A virtual disk on a SCSI2SD V5 card; the layout comes from a scsi2sd-util XML file."},
                          [limits] {
                              return blocks([] { return select_device(); }, [] { return scsi2sd_xml(Board::v5); },
                                            [limits] { return select_volume(limits.max_volume_bytes); });
                          }));
    (void)r.add(make_mode(
        {"scsi2sd-v6-image", "SCSI2SD V6 card image", "A virtual disk in an image of a SCSI2SD V6 card."},
        [] { return blocks([] { return select_image(512u); }, scsi2sd_v6, [] { return select_volume(); }); }));
    (void)r.add(make_mode({"scsi2sd-xml-image", "SCSI2SD card image with XML layout",
                           "A virtual disk in a SCSI2SD card image; the layout comes from a scsi2sd-util XML file."},
                          [] {
                              return blocks([] { return select_image(512u); }, [] { return scsi2sd_xml(); },
                                            [] { return select_volume(); });
                          }));
    return r;
}

} // namespace rawdisk::open
