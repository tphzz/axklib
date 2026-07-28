#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/io.hpp"

namespace axk::package_internal {

using Sha256Digest = std::array<std::byte, 32>;

class Sha256State {
  public:
    void update(std::span<const std::byte> bytes);
    [[nodiscard]] Sha256Digest finish();

  private:
    std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::byte, 64> block_{};
    std::size_t block_size_{};
    std::uint64_t total_bytes_{};
};

[[nodiscard]] Sha256Digest sha256(std::span<const std::byte> bytes);
[[nodiscard]] Result<Sha256Digest> sha256_reader(const RandomAccessReader &reader,
                                                 const CancellationToken &cancellation = {});
[[nodiscard]] std::string hex_digest(const Sha256Digest &digest);
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> bytes);

struct ArchiveEntry {
    std::string path;
    std::vector<std::byte> bytes;

    friend bool operator==(const ArchiveEntry &, const ArchiveEntry &) = default;
};

struct ArchiveLimits {
    std::size_t maximum_entries{4096U};
    std::uint64_t maximum_entry_bytes{64U * 1024U * 1024U};
    std::uint64_t maximum_total_bytes{512U * 1024U * 1024U};
    std::uint64_t maximum_archive_bytes{512U * 1024U * 1024U};
    std::uint64_t maximum_directory_bytes{16U * 1024U * 1024U};
    std::uint64_t maximum_manifest_bytes{8U * 1024U * 1024U};
};

struct ArchiveEntryInfo {
    std::string path;
    std::uint32_t checksum{};
    std::uint64_t size{};
    std::uint64_t data_offset{};

    friend bool operator==(const ArchiveEntryInfo &, const ArchiveEntryInfo &) = default;
};

struct ArchiveInspection {
    ArchiveEntry manifest;
    std::vector<ArchiveEntryInfo> entries;
    std::uint64_t archive_size{};
};

[[nodiscard]] Result<std::vector<std::byte>> write_archive(std::vector<ArchiveEntry> entries,
                                                           const ArchiveLimits &limits = {});

[[nodiscard]] Result<std::vector<ArchiveEntry>> read_archive(std::span<const std::byte> archive,
                                                             const ArchiveLimits &limits = {});

[[nodiscard]] Result<ArchiveInspection> inspect_archive(const RandomAccessReader &reader,
                                                        const CancellationToken &cancellation = {},
                                                        const ArchiveLimits &limits = {});

} // namespace axk::package_internal
