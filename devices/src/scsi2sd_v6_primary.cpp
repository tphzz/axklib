#include "rawdisk/scsi2sd/v6.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include "detail.hpp"

namespace rawdisk::scsi2sd::v6 {

Result<Config> read_primary(BlockDevice &card) noexcept {
    return rawdisk::detail::guard([&]() -> Result<Config> {
        if (card.sector_size() != kCardSectorSize)
            return rawdisk::detail::fail(Errc::invalid_argument);
        const auto end = config_lba(card.sector_count());
        if (!end)
            return rawdisk::detail::fail(Errc::layout_invalid);
        const auto raw = read_config(card, *end);
        if (!raw)
            return rawdisk::detail::fail(raw.error());
        auto config = decode(*raw);
        if (!config)
            return config;
        std::array<bool, 8> ids{};
        std::array<Range, kMaxTargets> ranges{};
        std::size_t count{};
        for (const auto &target : config->targets) {
            if (!target.enabled)
                continue;
            if (target.device_type > static_cast<std::uint8_t>(DeviceType::sequential) ||
                target.bytes_per_sector < kCardSectorSize || target.bytes_per_sector > 8192U ||
                target.bytes_per_sector % kCardSectorSize != 0U || target.scsi_sectors == 0U || ids[target.scsi_id])
                return rawdisk::detail::fail(Errc::layout_invalid);
            ids[target.scsi_id] = true;
            const std::uint64_t begin = target.sd_sector_start;
            const auto length = target.card_sectors();
            if (begin >= *end || length > *end - begin)
                return rawdisk::detail::fail(Errc::layout_invalid);
            const Range range{begin, begin + length};
            for (std::size_t i = 0; i < count; ++i)
                if (ranges[i].overlaps(range))
                    return rawdisk::detail::fail(Errc::layout_invalid);
            ranges[count++] = range;
        }
        if (count == 0U)
            return rawdisk::detail::fail(Errc::layout_invalid);
        return config;
    });
}

} // namespace rawdisk::scsi2sd::v6
