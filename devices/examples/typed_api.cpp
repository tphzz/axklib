// The typed API, step by step and without flows: find the reader, open it, read the SCSI2SD V6
// configuration from the last 2 sectors, turn it into volumes and open one of them.
//
//   typed_api              first USB disk
//   typed_api card.img     a card image instead (for testing without hardware)
#include "rawdisk/block_device.hpp"
#include "rawdisk/ownership.hpp"
#include "rawdisk/scsi2sd.hpp"

#include <cstdio>
#include <format>
#include <memory>

using namespace rawdisk;
namespace v6 = rawdisk::scsi2sd::v6;

static int fail(const char *what, std::error_code ec) {
    std::fprintf(stderr, "%s: %s\n", what, ec.message().c_str());
    return 1;
}

int main(int argc, char **argv) {
    // 1. Find and open the card, read-only.
    std::shared_ptr<BlockDevice> card;
    if (argc > 1) {
        auto img = open_image(argv[1], Access::ReadOnly);
        if (!img)
            return fail("open image", img.error());
        card = std::move(*img);
    } else {
        auto devices = list_usb_devices();
        if (!devices)
            return fail("list", devices.error());
        if (devices->empty())
            return fail("list", std::make_error_code(std::errc::no_such_device));
        const DeviceInfo &info = devices->front();
        std::printf("Reader: %s (%s)\n", info.model.c_str(), info.path.c_str());
        auto dev = open_owned_device(info, Access::ReadOnly); // may show the administrator prompt
        if (!dev)
            return fail("open", dev.error());
        card = std::move(*dev);
    }
    const std::uint64_t n = card->sector_count();

    // 2. The configuration lives in the last 2 sectors.
    const auto lba = v6::config_lba(n);
    if (!lba)
        return fail("card", make_error_code(Errc::layout_not_found));
    auto raw = v6::read_config(*card, *lba);
    if (!raw)
        return fail("read config", raw.error());

    // 3. Decode the full-fidelity format model.
    auto config = v6::decode(*raw);
    if (!config)
        return fail("decode (no SCSI2SD V6 configuration at the end of the card?)", config.error());
    std::printf("Board: flags 0x%02x, selection delay %u\n", config->board.flags, config->board.selection_delay);
    for (const auto &t : config->targets)
        if (t.enabled)
            std::printf("  SCSI ID %u: %s %s, start %u, %u x %u bytes\n", t.scsi_id, t.vendor.c_str(),
                        t.product.c_str(), t.sd_sector_start, t.scsi_sectors, t.bytes_per_sector);

    // 4. The generic view: volumes with findings.
    const VolumeSet set = v6::to_volumes(*config, n);
    for (const Notice &note : set.notices)
        std::printf("  [%s] %s\n", to_string(note.severity).data(), note.text.c_str());
    for (const Volume &v : set.volumes) {
        std::printf("Volume %s: %s%s\n", v.id.c_str(), v.label.c_str(), v.usable ? "" : " (not usable)");
        for (const Notice &note : v.notices)
            std::printf("  [%s] %s\n", to_string(note.severity).data(), note.text.c_str());
    }

    // 5. Open the first usable volume and read its first sector.
    for (const Volume &v : set.volumes) {
        if (!v.usable)
            continue;
        auto disk = open_volume(card, set, v.id, Access::ReadOnly);
        if (!disk)
            return fail("open volume", disk.error());
        AlignedBuffer sector((*disk)->sector_size());
        if (auto r = (*disk)->read(0, sector.span()); !r)
            return fail("read", r.error());
        std::printf("Read sector 0 of SCSI ID %s (%u bytes/sector, %llu sectors)\n", v.id.c_str(),
                    (*disk)->sector_size(), static_cast<unsigned long long>((*disk)->sector_count()));
        return 0;
    }
    std::printf("No usable volume.\n");
    return 0;
}
