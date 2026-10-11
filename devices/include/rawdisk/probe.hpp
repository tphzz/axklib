// Filesystem-independent, reversible single-sector write probes. No device discovery occurs here.
#pragma once

#include "rawdisk/block_device.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace rawdisk::probe {

struct Snapshot {
    std::uint64_t lba = 0;
    std::vector<std::byte> bytes{};
    friend bool operator==(const Snapshot &, const Snapshot &) = default;
};

struct Plan {
    std::uint32_t sector_size = 0;
    std::uint64_t sector_count = 0;
    std::uint64_t lba = 0;
    std::vector<std::byte> original{};
    std::vector<Snapshot> guards{};
    friend bool operator==(const Plan &, const Plan &) = default;
};

struct Identity {
    std::string locator{};
    std::optional<DeviceInfo> usb{};
    std::uint64_t offset_bytes = 0;
    friend bool operator==(const Identity &, const Identity &) = default;
};

struct Journal {
    Plan plan;
    Identity identity;
    std::vector<std::byte> modified;
    friend bool operator==(const Journal &, const Journal &) = default;
};

using Cancelled = std::function<bool()>;

struct Report {
    std::error_code primary_error{};
    std::error_code restoration_error{};
    bool write_attempted = false;
    bool pattern_verified = false;
    bool restored = false;
    bool ok() const noexcept { return !primary_error && !restoration_error && restored; }
};

// Bounded, checksummed binary format; see probe-format.md. Checksums detect damage, not tampering.
Result<void> validate(const Plan &plan) noexcept;
Result<Plan> load_plan(const std::filesystem::path &file) noexcept;
Result<void> save_plan(const std::filesystem::path &new_file, const Plan &plan) noexcept;
Result<Journal> load_journal(const std::filesystem::path &directory) noexcept;

// Read-only preflight: geometry, ten identical candidate reads and every guard must match.
Result<void> preflight(BlockDevice &device, const Plan &plan, Cancelled cancelled = {}) noexcept;

// Must receive the exclusively opened, identity-checked target. Adds immediate-neighbor guards,
// durably creates a NEW journal directory before attempting any write, rechecks context, flips
// the first byte's low bit, flushes/verifies, then restores/flushes/verifies on this SAME handle.
// Cancellation is cooperative; after a write attempt restoration is mandatory and uncancellable.
// Forced termination/power loss requires explicit recovery. Never reopens a removed device.
Report run(BlockDevice &device, const Plan &plan, const Identity &identity, const std::filesystem::path &new_journal,
           Cancelled cancelled = {}) noexcept;

// Call only after fresh identity-bound user confirmation. Refuses changed guards or a candidate
// other than original/modified. Already restored is a read-only no-op.
Report recover(BlockDevice &device, const Journal &journal, Cancelled cancelled = {}) noexcept;

} // namespace rawdisk::probe
