#include "detail.hpp"
#include "probe_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <new>
#include <optional>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace rawdisk::probe {
namespace {
std::error_code changed() { return make_error_code(Errc::device_changed); }
std::error_code canceled() { return std::make_error_code(std::errc::operation_canceled); }

Result<void> geometry(const BlockDevice &d, const Plan &p) {
    if (auto r = validate(p); !r)
        return r;
    if (d.sector_size() != p.sector_size || d.sector_count() != p.sector_count)
        return std::unexpected(changed());
    return {};
}

Result<void> same(BlockDevice &d, std::uint64_t lba, std::span<const std::byte> expected) {
    AlignedBuffer buf(expected.size());
    if (auto r = d.read(lba, buf.span()); !r)
        return r;
    if (!std::ranges::equal(buf.span(), expected))
        return std::unexpected(changed());
    return {};
}

Result<void> guards(BlockDevice &d, const Plan &p, const Cancelled &stop = {}) {
    for (const auto &g : p.guards) {
        if (stop && stop())
            return std::unexpected(canceled());
        if (auto r = same(d, g.lba, g.bytes); !r)
            return r;
    }
    return {};
}

void restore(BlockDevice &d, const Journal &j, Report &report) noexcept {
    // Even a failed/partial write may have reached the medium. Restoration never observes cancel.
    try {
        const auto written = d.write(j.plan.lba, j.plan.original);
        const auto flushed = d.flush();
        const auto verified = same(d, j.plan.lba, j.plan.original);
        const auto context = guards(d, j.plan);
        if (!written)
            report.restoration_error = written.error();
        else if (!flushed)
            report.restoration_error = flushed.error();
        else if (!verified)
            report.restoration_error = verified.error();
        else if (!context)
            report.restoration_error = context.error();
        report.restored = !report.restoration_error;
    } catch (...) {
        report.restoration_error = make_error_code(Errc::internal_error);
    }
}

Result<void> add_neighbors(BlockDevice &d, Plan &p) {
    for (const auto lba : {p.lba == 0 ? p.lba : p.lba - 1, p.lba + 1}) {
        if (lba == p.lba || lba >= p.sector_count)
            continue;
        if (std::ranges::any_of(p.guards, [&](const Snapshot &g) {
                return lba >= g.lba && lba - g.lba < g.bytes.size() / p.sector_size;
            }))
            continue;
        Snapshot s{lba, std::vector<std::byte>(p.sector_size)};
        if (auto r = d.read(lba, s.bytes); !r)
            return r;
        p.guards.push_back(std::move(s));
    }
    return validate(p);
}
} // namespace

Result<void> preflight(BlockDevice &d, const Plan &p, Cancelled stop) noexcept {
    return rawdisk::detail::guard([&]() -> Result<void> {
        if (auto r = geometry(d, p); !r)
            return r;
        for (int i = 0; i < 10; ++i) {
            if (stop && stop())
                return std::unexpected(canceled());
            if (auto r = same(d, p.lba, p.original); !r)
                return r;
        }
        return guards(d, p, stop);
    });
}

Report run(BlockDevice &d, const Plan &p, const Identity &identity, const std::filesystem::path &dir,
           Cancelled stop) noexcept {
    Report report;
    std::optional<Journal> journal;
    try {
        auto prepare = [&]() -> Result<void> {
            if (!d.writable())
                return std::unexpected(make_error_code(Errc::read_only_handle));
            if (auto r = preflight(d, p, stop); !r)
                return r;
            journal = Journal{p, identity, p.original};
            journal->modified[0] ^= std::byte{1};
            if (auto r = add_neighbors(d, journal->plan); !r)
                return r;
            if (auto r = detail::save_journal(dir, *journal); !r)
                return r;
            const auto persisted = load_journal(dir);
            if (!persisted)
                return std::unexpected(persisted.error());
            if (*persisted != *journal)
                return std::unexpected(changed());
            if (auto r = guards(d, journal->plan, stop); !r)
                return r;
            if (auto r = same(d, p.lba, p.original); !r)
                return r;
            if (stop && stop())
                return std::unexpected(canceled());
            // This flag must precede the call: an error return does not imply no bytes were written.
            report.write_attempted = true;
            if (auto r = d.write(p.lba, journal->modified); !r)
                return r;
            if (auto r = d.flush(); !r)
                return r;
            if (auto r = same(d, p.lba, journal->modified); !r)
                return r;
            if (auto r = guards(d, journal->plan); !r)
                return r;
            report.pattern_verified = true;
            if (stop && stop())
                return std::unexpected(canceled());
            return {};
        };
        if (auto r = prepare(); !r)
            report.primary_error = r.error();
    } catch (const std::bad_alloc &) {
        report.primary_error = std::make_error_code(std::errc::not_enough_memory);
    } catch (...) {
        report.primary_error = make_error_code(Errc::internal_error);
    }
    if (report.write_attempted && journal)
        restore(d, *journal, report);
    return report;
}

Report recover(BlockDevice &d, const Journal &j, Cancelled stop) noexcept {
    Report report;
    try {
        if (auto r = geometry(d, j.plan); !r) {
            report.primary_error = r.error();
            return report;
        }
        auto modified = j.plan.original;
        modified[0] ^= std::byte{1};
        if (modified != j.modified) {
            report.primary_error = make_error_code(Errc::invalid_argument);
            return report;
        }
        if (auto r = guards(d, j.plan, stop); !r) {
            report.primary_error = r.error();
            return report;
        }
        AlignedBuffer buf(j.plan.sector_size);
        if (auto r = d.read(j.plan.lba, buf.span()); !r) {
            report.primary_error = r.error();
            return report;
        }
        if (std::ranges::equal(buf.span(), j.plan.original)) {
            report.restored = true;
            return report;
        }
        if (!std::ranges::equal(buf.span(), j.modified)) {
            report.primary_error = changed();
            return report;
        }
        if (stop && stop()) {
            report.primary_error = canceled();
            return report;
        }
        if (!d.writable()) {
            report.primary_error = make_error_code(Errc::read_only_handle);
            return report;
        }
        report.write_attempted = true;
        restore(d, j, report);
    } catch (...) {
        report.primary_error = make_error_code(Errc::internal_error);
        if (report.write_attempted)
            restore(d, j, report);
    }
    return report;
}
} // namespace rawdisk::probe
