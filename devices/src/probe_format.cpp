#include "detail.hpp"
#include "probe_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace rawdisk::probe {
namespace {
constexpr std::string_view kPlanMagic = "RAWPLAN!";
constexpr std::string_view kJournalMagic = "RAWJRNL!";

std::uint32_t crc32(std::span<const std::byte> bytes) {
    std::uint32_t crc = 0xffffffffu;
    for (const auto byte : bytes) {
        crc ^= std::to_integer<std::uint32_t>(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    return crc ^ 0xffffffffu;
}

struct Writer {
    std::vector<std::byte> bytes;
    void number(std::uint64_t v, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i)
            bytes.push_back(static_cast<std::byte>((v >> (8 * i)) & 255));
    }
    void data(std::span<const std::byte> b) { bytes.insert(bytes.end(), b.begin(), b.end()); }
    void text(std::string_view s) {
        number(s.size(), 4);
        data(std::as_bytes(std::span(s.data(), s.size())));
    }
    void magic(std::string_view s) { data(std::as_bytes(std::span(s.data(), s.size()))); }
    void checksum() { number(crc32(bytes), 4); }
};

struct Reader {
    std::span<const std::byte> bytes;
    std::size_t pos = 0;
    bool ok = true;
    std::span<const std::byte> data(std::size_t n) {
        if (!ok || n > bytes.size() - pos) {
            ok = false;
            return {};
        }
        const auto s = bytes.subspan(pos, n);
        pos += n;
        return s;
    }
    std::uint64_t number(std::size_t n) {
        const auto s = data(n);
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < s.size(); ++i)
            v |= std::to_integer<std::uint64_t>(s[i]) << (8 * i);
        return v;
    }
    std::vector<std::byte> blob(std::size_t n) {
        const auto s = data(n);
        return {s.begin(), s.end()};
    }
    std::string text() {
        const auto n = number(4);
        if (n > 8192) {
            ok = false;
            return {};
        }
        const auto s = data(static_cast<std::size_t>(n));
        std::string t;
        for (auto b : s)
            t += static_cast<char>(std::to_integer<unsigned char>(b));
        if (t.find('\0') != std::string::npos)
            ok = false;
        return t;
    }
};

void put_plan(Writer &w, const Plan &p) {
    w.number(p.sector_size, 4);
    w.number(p.sector_count, 8);
    w.number(p.lba, 8);
    w.number(p.guards.size(), 4);
    w.data(p.original);
    for (const auto &g : p.guards) {
        w.number(g.lba, 8);
        w.number(g.bytes.size(), 4);
        w.data(g.bytes);
    }
}

Plan get_plan(Reader &r) {
    Plan p;
    p.sector_size = static_cast<std::uint32_t>(r.number(4));
    p.sector_count = r.number(8);
    p.lba = r.number(8);
    const auto guards = r.number(4);
    if (!rawdisk::detail::valid_sector_size(p.sector_size) || guards > detail::kMaxGuards) {
        r.ok = false;
        return p;
    }
    p.original = r.blob(p.sector_size);
    for (std::uint64_t i = 0; r.ok && i < guards; ++i) {
        const auto lba = r.number(8);
        const auto n = r.number(4);
        if (n > detail::kMaxBytes) {
            r.ok = false;
            break;
        }
        p.guards.push_back({lba, r.blob(static_cast<std::size_t>(n))});
    }
    return p;
}

Result<std::vector<std::byte>> checked_file(const std::filesystem::path &path, std::string_view magic) {
    auto b = detail::read_file(path);
    if (!b)
        return std::unexpected(b.error());
    if (b->size() < 12 || std::memcmp(b->data(), magic.data(), 8) != 0)
        return std::unexpected(make_error_code(Errc::invalid_argument));
    Reader tail{std::span(*b).last(4)};
    if (crc32(std::span(*b).first(b->size() - 4)) != tail.number(4))
        return std::unexpected(make_error_code(Errc::invalid_argument));
    b->resize(b->size() - 4);
    return b;
}
} // namespace

Result<void> validate(const Plan &p) noexcept {
    return rawdisk::detail::guard([&]() -> Result<void> {
        if (!rawdisk::detail::valid_sector_size(p.sector_size) || p.sector_count == 0 ||
            p.sector_count > std::numeric_limits<std::uint64_t>::max() / p.sector_size || p.lba >= p.sector_count ||
            p.original.size() != p.sector_size || p.guards.size() > detail::kMaxGuards)
            return std::unexpected(make_error_code(Errc::invalid_argument));
        std::size_t total = p.original.size();
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges{{p.lba, p.lba + 1}};
        for (const auto &g : p.guards) {
            if (g.bytes.empty() || g.bytes.size() % p.sector_size != 0 || g.lba > p.sector_count ||
                g.bytes.size() / p.sector_size > p.sector_count - g.lba || g.bytes.size() > detail::kMaxBytes - total)
                return std::unexpected(make_error_code(Errc::invalid_argument));
            total += g.bytes.size();
            ranges.emplace_back(g.lba, g.lba + g.bytes.size() / p.sector_size);
        }
        std::ranges::sort(ranges);
        for (std::size_t i = 1; i < ranges.size(); ++i)
            if (ranges[i - 1].second > ranges[i].first)
                return std::unexpected(make_error_code(Errc::invalid_argument));
        return {};
    });
}

Result<void> save_plan(const std::filesystem::path &path, const Plan &p) noexcept {
    return rawdisk::detail::guard([&]() -> Result<void> {
        if (auto r = validate(p); !r)
            return r;
        Writer w;
        w.magic(kPlanMagic);
        put_plan(w, p);
        w.checksum();
        return detail::write_new(path, w.bytes);
    });
}

Result<Plan> load_plan(const std::filesystem::path &path) noexcept {
    return rawdisk::detail::guard([&]() -> Result<Plan> {
        auto bytes = checked_file(path, kPlanMagic);
        if (!bytes)
            return std::unexpected(bytes.error());
        Reader r{std::span(*bytes).subspan(8)};
        Plan p = get_plan(r);
        if (!r.ok || r.pos != r.bytes.size() || !validate(p))
            return std::unexpected(make_error_code(Errc::invalid_argument));
        return p;
    });
}

Result<void> detail::save_journal(const std::filesystem::path &dir, const Journal &j) {
    const auto valid_text = [](const std::string &text) {
        return text.size() <= 8192 && text.find('\0') == std::string::npos;
    };
    if (!valid_text(j.identity.locator) ||
        (j.identity.usb && (!valid_text(j.identity.usb->path) || !valid_text(j.identity.usb->model) ||
                            !valid_text(j.identity.usb->serial))))
        return std::unexpected(make_error_code(Errc::invalid_argument));
    if (auto r = validate(j.plan); !r)
        return r;
    Writer w;
    w.magic(kJournalMagic);
    w.text(j.identity.locator);
    w.number(j.identity.offset_bytes, 8);
    w.number(j.identity.usb.has_value() ? 1 : 0, 1);
    if (j.identity.usb) {
        const auto &d = *j.identity.usb;
        w.text(d.path);
        w.text(d.model);
        w.text(d.serial);
        w.number(d.size_bytes, 8);
        w.number(d.sector_size, 4);
        w.number(d.removable ? 1 : 0, 1);
    }
    put_plan(w, j.plan);
    w.data(j.modified);
    w.checksum();
    std::error_code ec;
    if (!std::filesystem::create_directory(dir, ec))
        return std::unexpected(ec ? ec : std::make_error_code(std::errc::file_exists));
    if (auto r = write_new(dir / "journal.bin", w.bytes); !r)
        return r;
    if (auto r = sync_directory(dir.has_parent_path() ? dir.parent_path() : std::filesystem::path(".")); !r)
        return r;
    return publication_barrier(dir / "journal.bin");
}

Result<Journal> load_journal(const std::filesystem::path &dir) noexcept {
    return rawdisk::detail::guard([&]() -> Result<Journal> {
        auto bytes = checked_file(dir / "journal.bin", kJournalMagic);
        if (!bytes)
            return std::unexpected(bytes.error());
        Reader r{std::span(*bytes).subspan(8)};
        Journal j;
        j.identity.locator = r.text();
        j.identity.offset_bytes = r.number(8);
        const auto usb = r.number(1);
        if (usb > 1)
            r.ok = false;
        if (usb == 1) {
            DeviceInfo d;
            d.path = r.text();
            d.model = r.text();
            d.serial = r.text();
            d.size_bytes = r.number(8);
            d.sector_size = static_cast<std::uint32_t>(r.number(4));
            const auto removable = r.number(1);
            if (removable > 1)
                r.ok = false;
            d.removable = removable == 1;
            j.identity.usb = std::move(d);
        }
        j.plan = get_plan(r);
        if (!validate(j.plan))
            r.ok = false;
        j.modified = r.blob(j.plan.original.size());
        auto expected = j.plan.original;
        if (!expected.empty())
            expected[0] ^= std::byte{1};
        if (!r.ok || r.pos != r.bytes.size() || j.modified != expected)
            return std::unexpected(make_error_code(Errc::invalid_argument));
        return j;
    });
}
} // namespace rawdisk::probe
