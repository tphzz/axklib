#include "rawdisk/scsi2sd/v6.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <random>
#include <span>
#include <system_error>

using namespace rawdisk;
namespace v6 = rawdisk::scsi2sd::v6;

namespace {
class Card final : public BlockDevice {
  public:
    explicit Card(std::uint64_t sectors = 8192) : sectors_(sectors) {}
    std::uint32_t sector_size() const noexcept override { return sector_size_; }
    std::uint64_t sector_count() const noexcept override { return sectors_; }
    bool writable() const noexcept override { return false; }
    Result<void> read_raw(std::uint64_t offset, std::span<std::byte> out) override {
        ++reads;
        last_lba = offset / sector_size_;
        if (read_error)
            return std::unexpected(std::make_error_code(std::errc::io_error));
        if (out.size() != raw.size())
            return std::unexpected(make_error_code(Errc::invalid_argument));
        std::copy(raw.begin(), raw.end(), out.begin());
        return {};
    }
    Result<void> write_raw(std::uint64_t, std::span<const std::byte>) override {
        ++writes;
        return std::unexpected(make_error_code(Errc::read_only_handle));
    }
    Result<void> flush_raw() override { return {}; }
    std::uint64_t sectors_;
    std::uint32_t sector_size_{512};
    v6::RawConfig raw{};
    bool read_error{};
    unsigned reads{};
    unsigned writes{};
    std::uint64_t last_lba{};
};

v6::Config valid_config() {
    auto config = v6::default_config();
    config.targets[0].scsi_sectors = 2000;
    auto &optical = config.targets[1];
    optical.enabled = true;
    optical.scsi_id = 2;
    optical.device_type = 2;
    optical.sd_sector_start = 3000;
    optical.scsi_sectors = 1000;
    optical.bytes_per_sector = 2048;
    return config;
}

void test_primary() {
    Card card;
    const auto config = valid_config();
    card.raw = v6::encode(config);
    auto read = v6::read_primary(card);
    CHECK(read.has_value());
    CHECK(card.reads == 1U && card.last_lba == 8190U && card.writes == 0U);
    CHECK(read && v6::encode(*read) == card.raw);
    CHECK(read && read->targets[1].scsi_id == 2U && read->targets[1].byte_offset() == 1536000U &&
          read->targets[1].byte_size() == 2048000U);

    card.raw.fill(std::byte{});
    CHECK(err(v6::read_primary(card)) == Errc::layout_not_found);
    card.raw.fill(std::byte{0xFF});
    CHECK(err(v6::read_primary(card)) == Errc::layout_not_found);
    card.raw = v6::encode(config);
    card.read_error = true;
    CHECK(err(v6::read_primary(card)) == std::errc::io_error);
    card.read_error = false;
    card.sector_size_ = 4096;
    CHECK(err(v6::read_primary(card)) == Errc::invalid_argument);
    card.sector_size_ = 512;
    card.sectors_ = 1;
    CHECK(err(v6::read_primary(card)) == Errc::layout_invalid);
}

void test_invalid_geometry() {
    const auto base = valid_config();
    const auto invalid = [&](const v6::Config &config) {
        Card card;
        card.raw = v6::encode(config);
        CHECK(err(v6::read_primary(card)) == Errc::layout_invalid);
        CHECK(card.writes == 0U);
    };
    auto config = base;
    config.targets[0].scsi_sectors = 0;
    invalid(config);
    for (const auto sector : std::array<std::uint16_t, 5>{0, 256, 768, 8193, 65535}) {
        config = base;
        config.targets[0].bytes_per_sector = sector;
        invalid(config);
    }
    config = base;
    config.targets[0].device_type = 255;
    invalid(config);
    config = base;
    config.targets[1].scsi_id = config.targets[0].scsi_id;
    invalid(config);
    config = base;
    config.targets[1].sd_sector_start = 1999;
    invalid(config);
    config = base;
    config.targets[1].sd_sector_start = 8192;
    invalid(config);
    config = base;
    config.targets[1].sd_sector_start = 8190;
    config.targets[1].scsi_sectors = 1;
    config.targets[1].bytes_per_sector = 512;
    invalid(config);
    config = base;
    config.targets[0].sd_sector_start = std::numeric_limits<std::uint32_t>::max();
    config.targets[0].scsi_sectors = std::numeric_limits<std::uint32_t>::max();
    config.targets[0].bytes_per_sector = 8192;
    invalid(config);
    for (auto &target : config.targets)
        target.enabled = false;
    invalid(config);

    config = base;
    config.targets[1].sd_sector_start = 2000;
    Card adjacent;
    adjacent.raw = v6::encode(config);
    CHECK(v6::read_primary(adjacent).has_value());
    config.targets[2].scsi_sectors = std::numeric_limits<std::uint32_t>::max();
    config.targets[2].bytes_per_sector = 65535;
    config.targets[2].device_type = 255;
    adjacent.raw = v6::encode(config);
    CHECK(v6::read_primary(adjacent).has_value()); // Disabled slots are not geometry.
}

void test_unrelated_bytes() {
    std::mt19937 random{237U};
    Card card;
    for (unsigned iteration = 0; iteration < 1000U; ++iteration) {
        for (auto &byte : card.raw)
            byte = static_cast<std::byte>(random() & 0xFFU);
        // Even a coincidental magic string must not expose random target geometry.
        std::copy_n(v6::encode(valid_config()).begin(), 4, card.raw.begin());
        CHECK(err(v6::read_primary(card)) == Errc::layout_invalid);
    }
    CHECK(card.reads == 1000U && card.writes == 0U);
}
} // namespace

int main() {
    test_primary();
    test_invalid_geometry();
    test_unrelated_bytes();
    return g_failures ? 1 : 0;
}
