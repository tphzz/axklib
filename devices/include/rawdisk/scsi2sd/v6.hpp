// SCSI2SD V6: the configuration stored on the card, in its last 2 sectors (config.c,
// s2s_configInit): a 128-byte board block starting with "BCFG", then 7 target blocks of 128
// bytes, little-endian. Every step is available on its own; discover() composes them.
#pragma once

#include "rawdisk/block_device.hpp"
#include "rawdisk/cancellation.hpp"
#include "rawdisk/scsi2sd/common.hpp"
#include "rawdisk/volume.hpp"

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace rawdisk::scsi2sd::v6 {

inline constexpr std::size_t kMaxTargets = 7;
inline constexpr std::size_t kConfigBytes = 128 + kMaxTargets * 128;            // 1024
inline constexpr std::uint64_t kConfigSectors = kConfigBytes / kCardSectorSize; // 2
inline constexpr std::uint64_t kDefaultScanWindow = 16384;                      // 8 MiB

using RawConfig = std::array<std::byte, kConfigBytes>;

// S2S_BoardCfg.
struct Board {
    std::uint8_t flags = 0;           // kFlag*
    std::uint8_t startup_delay = 0;   // seconds
    std::uint8_t selection_delay = 0; // ms, 255 = auto
    std::uint8_t flags6 = 0;          // kFlag6*
    std::uint8_t scsi_speed = 0;
    std::array<std::byte, 128> raw{};
};

struct Config {
    Board board;
    std::array<Target, kMaxTargets> targets;
};

// Where the firmware reads the configuration: the last 2 sectors. nullopt if the card is smaller.
std::optional<std::uint64_t> config_lba(std::uint64_t card_sectors) noexcept;
// Where firmware 0x0632's s2s_configSave() writes it by mistake (capacity - S2S_CFG_SIZE
// *sectors*, i.e. 1024 sectors before the end) after a host MODE SELECT. nullopt if too small.
std::optional<std::uint64_t> misplaced_save_lba(std::uint64_t card_sectors) noexcept;

// The 1024 raw bytes at `lba`, uninterpreted. The card must have 512-byte sectors.
Result<RawConfig> read_config(BlockDevice &card, std::uint64_t lba) noexcept;

// Errc::layout_not_found without the "BCFG" magic, Errc::layout_invalid if shorter than 1024.
Result<Config> decode(std::span<const std::byte> bytes) noexcept;
// Strict application admission: primary block only, recognized enabled targets,
// supported whole-sector packing and non-overlapping ranges before the config.
// Never scans, truncates targets, or substitutes the firmware default.
Result<Config> read_primary(BlockDevice &card) noexcept;
// Inverse of decode: encode(decode(b)) == b for every valid block, byte for byte.
RawConfig encode(const Config &config) noexcept;

// What the firmware uses when the card holds no configuration: one disk, SCSI ID 0,
// card sector 0, 4194303 sectors of 512 bytes.
Config default_config() noexcept;

enum class CandidateKind {
    primary,        // last 2 sectors: what the firmware reads
    misplaced_save, // 1024 sectors before the end: left by the MODE SELECT save bug, never read
    elsewhere,      // elsewhere in the scan window: the board may detect a smaller capacity
};
std::string_view to_string(CandidateKind k) noexcept;

struct Candidate {
    std::uint64_t lba = 0;
    CandidateKind kind = CandidateKind::primary;
    Config config;
};

struct ScanOptions {
    enum class Scan { never, if_no_primary, always };
    Scan scan = Scan::if_no_primary;
    std::uint64_t window_sectors = kDefaultScanWindow; // searched from the end
    std::size_t max_candidates = 16;
    CancellationToken stop{}; // cancels with std::errc::operation_canceled
};

// Every configuration block found: primary first, then the misplaced-save copy, then others
// from the end backwards.
Result<std::vector<Candidate>> find_configs(BlockDevice &card, const ScanOptions &options = {}) noexcept;

// The generic volume view: enabled targets as volumes (id = SCSI ID), cut at the card end, with
// findings as notices. The last 2 card sectors, and `config_at` if it differs, are protected.
VolumeSet to_volumes(const Config &config, std::uint64_t card_sectors,
                     std::optional<std::uint64_t> config_at = std::nullopt);

// What the board will actually use, composed from the functions above.
struct Discovery {
    std::vector<Candidate> candidates;
    std::optional<std::size_t> used; // index into candidates; nullopt: firmware default
    Config effective;
    VolumeSet volumes;           // of `effective`, notices included
    std::vector<Notice> notices; // about the candidates (also copied into volumes.notices)
    bool from_card() const noexcept { return used.has_value(); }
};
Result<Discovery> discover(BlockDevice &card, const ScanOptions &options = {}) noexcept;

} // namespace rawdisk::scsi2sd::v6
