#include "detail.hpp"
#include "rawdisk/scsi2sd/v6.hpp"
#include "scsi2sd_common.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace rawdisk::scsi2sd::v6 {
namespace {

using rawdisk::detail::fail;

constexpr std::size_t kBlock = 128;
constexpr std::uint64_t kMisplacedOffset = kConfigBytes; // S2S_CFG_SIZE used as a sector count

std::uint16_t le16(std::span<const std::byte> b, std::size_t o) {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(b[o]) | (std::to_integer<unsigned>(b[o + 1]) << 8));
}
std::uint32_t le32(std::span<const std::byte> b, std::size_t o) {
    return std::uint32_t{le16(b, o)} | (std::uint32_t{le16(b, o + 2)} << 16);
}
void put16(std::span<std::byte> b, std::size_t o, std::uint16_t v) {
    b[o] = static_cast<std::byte>(v & 0xFF);
    b[o + 1] = static_cast<std::byte>(v >> 8);
}
void put32(std::span<std::byte> b, std::size_t o, std::uint32_t v) {
    put16(b, o, static_cast<std::uint16_t>(v & 0xFFFF));
    put16(b, o + 2, static_cast<std::uint16_t>(v >> 16));
}

// Fixed-width, space/NUL padded ASCII -> trimmed printable string.
std::string get_str(std::span<const std::byte> b, std::size_t o, std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        const auto c = std::to_integer<unsigned char>(b[o + i]);
        s += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : (c == 0 ? ' ' : '?');
    }
    return rawdisk::detail::trim(s);
}
// Keeps the stored bytes if the text is unchanged (bit-exact round trip), else writes the new
// text space-padded and truncated to the field width.
void put_str(std::span<std::byte> b, std::size_t o, std::size_t n, const std::string &s) {
    if (get_str(b, o, n) == s)
        return;
    for (std::size_t i = 0; i < n; ++i)
        b[o + i] = static_cast<std::byte>(i < s.size() ? s[i] : ' ');
}

bool has_magic(std::span<const std::byte> b) { return b.size() >= 4 && std::memcmp(b.data(), "BCFG", 4) == 0; }

std::string describe(const Candidate &c, std::uint64_t n) {
    return std::format("card sector {} ({} sectors before the end)", c.lba, n - c.lba);
}

} // namespace

std::optional<std::uint64_t> config_lba(std::uint64_t n) noexcept {
    if (n < kConfigSectors)
        return std::nullopt;
    return n - kConfigSectors;
}

std::optional<std::uint64_t> misplaced_save_lba(std::uint64_t n) noexcept {
    if (n < kMisplacedOffset)
        return std::nullopt;
    return n - kMisplacedOffset;
}

std::string_view to_string(CandidateKind k) noexcept {
    switch (k) {
    case CandidateKind::primary:
        return "primary";
    case CandidateKind::misplaced_save:
        return "misplaced save";
    case CandidateKind::elsewhere:
        return "elsewhere";
    }
    return "unknown";
}

Result<RawConfig> read_config(BlockDevice &card, std::uint64_t lba) noexcept {
    return rawdisk::detail::guard([&]() -> Result<RawConfig> {
        if (card.sector_size() != kCardSectorSize)
            return fail(Errc::invalid_argument);
        AlignedBuffer buf(kConfigBytes);
        if (auto r = card.read(lba, buf.span()); !r)
            return fail(r.error());
        RawConfig out{};
        std::memcpy(out.data(), buf.data(), kConfigBytes);
        return out;
    });
}

Result<Config> decode(std::span<const std::byte> b) noexcept {
    return rawdisk::detail::guard([&]() -> Result<Config> {
        if (b.size() < kConfigBytes)
            return fail(Errc::layout_invalid);
        if (!has_magic(b))
            return fail(Errc::layout_not_found);
        Config c;
        std::copy_n(b.begin(), kBlock, c.board.raw.begin());
        c.board.flags = std::to_integer<std::uint8_t>(b[4]);
        c.board.startup_delay = std::to_integer<std::uint8_t>(b[5]);
        c.board.selection_delay = std::to_integer<std::uint8_t>(b[6]);
        c.board.flags6 = std::to_integer<std::uint8_t>(b[7]);
        c.board.scsi_speed = std::to_integer<std::uint8_t>(b[8]);
        for (std::size_t i = 0; i < kMaxTargets; ++i) {
            const std::size_t o = kBlock * (i + 1);
            Target &t = c.targets[i];
            std::copy_n(b.begin() + static_cast<std::ptrdiff_t>(o), kBlock, t.raw.begin());
            t.index = i;
            const auto id = std::to_integer<std::uint8_t>(b[o]);
            t.enabled = (id & 0x80) != 0;
            t.scsi_id = id & 0x07;
            t.device_type = std::to_integer<std::uint8_t>(b[o + 1]);
            t.flags_deprecated = std::to_integer<std::uint8_t>(b[o + 2]);
            t.device_type_modifier = std::to_integer<std::uint8_t>(b[o + 3]);
            t.sd_sector_start = le32(b, o + 4);
            t.scsi_sectors = le32(b, o + 8);
            t.bytes_per_sector = le16(b, o + 12);
            t.sectors_per_track = le16(b, o + 14);
            t.heads_per_cylinder = le16(b, o + 16);
            t.vendor = get_str(b, o + 18, 8);
            t.product = get_str(b, o + 26, 16);
            t.revision = get_str(b, o + 42, 4);
            t.serial = get_str(b, o + 46, 16);
            t.quirks = le16(b, o + 62);
        }
        return c;
    });
}

RawConfig encode(const Config &c) noexcept {
    RawConfig out{};
    std::span<std::byte> b(out);
    std::copy(c.board.raw.begin(), c.board.raw.end(), out.begin());
    std::memcpy(out.data(), "BCFG", 4);
    b[4] = std::byte{c.board.flags};
    b[5] = std::byte{c.board.startup_delay};
    b[6] = std::byte{c.board.selection_delay};
    b[7] = std::byte{c.board.flags6};
    b[8] = std::byte{c.board.scsi_speed};
    for (std::size_t i = 0; i < kMaxTargets; ++i) {
        const std::size_t o = kBlock * (i + 1);
        const Target &t = c.targets[i];
        std::copy(t.raw.begin(), t.raw.end(), out.begin() + static_cast<std::ptrdiff_t>(o));
        // Bits 6..3 of the ID byte are undefined flags: keep them as stored.
        const auto kept = static_cast<std::uint8_t>(std::to_integer<std::uint8_t>(b[o]) & 0x78);
        b[o] = std::byte{static_cast<std::uint8_t>(kept | (t.enabled ? 0x80 : 0) | (t.scsi_id & 0x07))};
        b[o + 1] = std::byte{t.device_type};
        b[o + 2] = std::byte{t.flags_deprecated};
        b[o + 3] = std::byte{t.device_type_modifier};
        put32(b, o + 4, t.sd_sector_start);
        put32(b, o + 8, t.scsi_sectors);
        put16(b, o + 12, t.bytes_per_sector);
        put16(b, o + 14, t.sectors_per_track);
        put16(b, o + 16, t.heads_per_cylinder);
        put_str(b, o + 18, 8, t.vendor);
        put_str(b, o + 26, 16, t.product);
        put_str(b, o + 42, 4, t.revision);
        put_str(b, o + 46, 16, t.serial);
        put16(b, o + 62, t.quirks);
    }
    return out;
}

Config default_config() noexcept {
    // DEFAULT_CONFIG in config.c, applied in s2s_configInit when the card has no "BCFG".
    Config c;
    c.board.selection_delay = 255;
    c.board.flags6 = kFlag6Terminator;
    std::memcpy(c.board.raw.data(), "BCFG", 4);
    for (std::size_t i = 0; i < kMaxTargets; ++i)
        c.targets[i].index = i;
    Target &t = c.targets[0];
    t.enabled = true;
    t.scsi_sectors = 0x003FFFFF;
    t.bytes_per_sector = 512;
    t.sectors_per_track = 63;
    t.heads_per_cylinder = 255;
    t.vendor = "codesrc";
    t.product = "SCSI2SD";
    t.revision = "1.0";
    t.serial = "1234567812345678";
    return c;
}

Result<std::vector<Candidate>> find_configs(BlockDevice &card, const ScanOptions &o) noexcept {
    return rawdisk::detail::guard([&]() -> Result<std::vector<Candidate>> {
        if (card.sector_size() != kCardSectorSize)
            return fail(Errc::invalid_argument);
        const std::uint64_t n = card.sector_count();
        std::vector<Candidate> found;
        auto try_at = [&](std::uint64_t lba, CandidateKind kind) -> Result<void> {
            auto raw = read_config(card, lba);
            if (!raw)
                return fail(raw.error());
            auto c = decode(*raw);
            if (c)
                found.push_back({lba, kind, std::move(*c)});
            else if (c.error() != Errc::layout_not_found)
                return fail(c.error());
            return {};
        };

        const auto primary = config_lba(n);
        if (!primary)
            return found;
        if (auto r = try_at(*primary, CandidateKind::primary); !r)
            return fail(r.error());
        const bool have_primary = !found.empty();
        const auto misplaced = misplaced_save_lba(n);
        if (misplaced) {
            if (auto r = try_at(*misplaced, CandidateKind::misplaced_save); !r)
                return fail(r.error());
        }

        const bool scan =
            o.scan == ScanOptions::Scan::always || (o.scan == ScanOptions::Scan::if_no_primary && !have_primary);
        if (!scan)
            return found;
        const std::uint64_t first = n > o.window_sectors ? n - o.window_sectors : 0;
        constexpr std::uint64_t kChunk = 2048; // 1 MiB per read
        AlignedBuffer chunk(kChunk * kCardSectorSize);
        for (std::uint64_t end = *primary; end > first && found.size() < o.max_candidates;) {
            if (o.stop.is_cancelled())
                return fail(std::make_error_code(std::errc::operation_canceled));
            const std::uint64_t begin = std::max(first, end > kChunk ? end - kChunk : 0);
            const auto span = chunk.span().first(static_cast<std::size_t>((end - begin) * kCardSectorSize));
            if (auto r = card.read(begin, span); !r)
                return fail(r.error());
            for (std::uint64_t lba = end; lba-- > begin && found.size() < o.max_candidates;) {
                if (misplaced && lba == *misplaced)
                    continue;
                if (!has_magic(span.subspan(static_cast<std::size_t>((lba - begin) * kCardSectorSize))))
                    continue;
                if (auto r = try_at(lba, CandidateKind::elsewhere); !r)
                    return fail(r.error());
            }
            end = begin;
        }
        return found;
    });
}

VolumeSet to_volumes(const Config &c, std::uint64_t n, std::optional<std::uint64_t> config_at) {
    std::vector<Range> protect;
    if (n >= kConfigSectors)
        protect.push_back({n - kConfigSectors, n});
    if (config_at && *config_at + kConfigSectors <= n && *config_at != n - kConfigSectors)
        protect.push_back({*config_at, *config_at + kConfigSectors});
    return detail::build_volumes("scsi2sd-v6", c.targets, n, true, std::move(protect));
}

Result<Discovery> discover(BlockDevice &card, const ScanOptions &o) noexcept {
    return rawdisk::detail::guard([&]() -> Result<Discovery> {
        auto found = find_configs(card, o);
        if (!found)
            return fail(found.error());
        const std::uint64_t n = card.sector_count();
        Discovery d;
        d.candidates = std::move(*found);

        auto index_of = [&](CandidateKind k) -> std::optional<std::size_t> {
            for (std::size_t i = 0; i < d.candidates.size(); ++i)
                if (d.candidates[i].kind == k)
                    return i;
            return std::nullopt;
        };
        const auto primary = index_of(CandidateKind::primary);
        const auto misplaced = index_of(CandidateKind::misplaced_save);
        const auto elsewhere = index_of(CandidateKind::elsewhere); // nearest to the end first

        if (primary) {
            d.used = primary;
        } else if (elsewhere) {
            d.used = elsewhere;
            d.notices.push_back(
                {Severity::danger, "s2s.config_elsewhere",
                 std::format("No configuration in the last 2 sectors, but one at {}. The board reads the "
                             "last 2 sectors of the capacity it detects itself, so it probably detects "
                             "a smaller card than this reader. Verify in scsi2sd-util before writing.",
                             describe(d.candidates[*elsewhere], n))});
        } else {
            d.notices.push_back({Severity::warning, "s2s.firmware_default",
                                 "No configuration on the card: the board runs with its built-in default "
                                 "(one disk, SCSI ID 0, card sector 0, 4194303 sectors of 512 bytes)."});
        }
        if (misplaced) {
            const bool same = d.used && encode(d.candidates[*d.used].config) == encode(d.candidates[*misplaced].config);
            if (!same)
                d.notices.push_back(
                    {Severity::warning, "s2s.misplaced_copy",
                     std::format("A configuration copy at {} was left by the firmware's MODE SELECT save "
                                 "bug. The board never reads it; a sector-size change made by the host "
                                 "was probably lost.",
                                 describe(d.candidates[*misplaced], n))});
        }

        d.effective = d.used ? d.candidates[*d.used].config : default_config();
        d.volumes = to_volumes(d.effective, n, d.used ? std::optional(d.candidates[*d.used].lba) : std::nullopt);
        d.volumes.notices.insert(d.volumes.notices.begin(), d.notices.begin(), d.notices.end());
        return d;
    });
}

} // namespace rawdisk::scsi2sd::v6
