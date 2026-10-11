// SCSI2SD: types shared by the V6 on-card format and the scsi2sd-util XML format.
// Sources: SCSI2SD V6 firmware include/scsi2sd.h and src/firmware/config.c (0x0632), V5 scsi2sd.h.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace rawdisk::scsi2sd {

// sd_sector_start is always in 512-byte card sectors, whatever the SCSI sector size.
inline constexpr std::uint32_t kCardSectorSize = 512;

enum class DeviceType : std::uint8_t { fixed = 0, removable = 1, optical = 2, floppy_14mb = 3, mo = 4, sequential = 5 };

// Board flags (S2S_CFG_FLAGS / S2S_CFG_FLAGS6).
inline constexpr std::uint8_t kFlagUnitAttention = 1, kFlagParity = 2, kFlagScsi2 = 4, kFlagDisableGlitch = 8,
                              kFlagCache = 16, kFlagDisconnect = 32, kFlagSelLatch = 64, kFlagMapLunsToIds = 128;
inline constexpr std::uint8_t kFlag6Terminator = 1, kFlag6BlindWrites = 2;
// Target quirks (S2S_CFG_QUIRKS).
inline constexpr std::uint16_t kQuirkApple = 1, kQuirkOmti = 2, kQuirkXebec = 4, kQuirkVms = 8;

// One configured SCSI target (S2S_TargetCfg).
struct Target {
    std::size_t index = 0;    // slot in the configuration, 0-based
    std::uint8_t scsi_id = 0; // 0..7
    bool enabled = false;
    std::uint8_t device_type = 0; // DeviceType
    std::uint8_t flags_deprecated = 0;
    std::uint8_t device_type_modifier = 0;
    std::uint32_t sd_sector_start = 0;
    std::uint32_t scsi_sectors = 0;
    std::uint16_t bytes_per_sector = 0;
    std::uint16_t sectors_per_track = 0;
    std::uint16_t heads_per_cylinder = 0;
    std::uint16_t quirks = 0;
    std::string vendor{}, product{}, revision{}, serial{}; // trimmed; non-printable bytes shown as '?'
    // The 128 bytes as stored (V6 cards only; zero for XML). v6::encode keeps every byte that
    // the decoded fields above do not change, including the reserved area.
    std::array<std::byte, 128> raw{};

    std::uint64_t byte_offset() const noexcept { return std::uint64_t{sd_sector_start} * kCardSectorSize; }
    std::uint64_t byte_size() const noexcept { return std::uint64_t{scsi_sectors} * bytes_per_sector; }
    std::uint64_t card_sectors() const noexcept { return (byte_size() + kCardSectorSize - 1) / kCardSectorSize; }
};

bool same_target_layout(std::span<const Target> first, std::span<const Target> second);

} // namespace rawdisk::scsi2sd
