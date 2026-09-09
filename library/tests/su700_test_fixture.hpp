#pragma once

#include "media_test_fixtures.hpp"
#include <string_view>
#include <utility>

namespace {
inline std::vector<std::byte> control_fixture() {
    std::vector<std::byte> bytes(7400);
    ascii(bytes, 0, "SONG    ");
    ascii(bytes, 9, "SAMPLE  ");
    bytes[369] = std::byte{1};
    ascii(bytes, 370, "UNUSED  ");
    return bytes;
}
inline std::vector<std::byte> su700_floppy_bytes(std::string sample_name = "SAMPLE", bool truncated = false,
                                                 bool generic = false, bool legacy_song = false) {
    auto bytes = fat_fixture();
    std::ranges::fill(std::span{bytes}.subspan(3U * 512U), std::byte{});
    std::vector<std::byte> song(legacy_song ? 16U : 22U);
    ascii(song, 0, "FLhd");
    be32(song, 4, static_cast<std::uint32_t>(song.size()));
    be32(song, 8, 0x16);
    ascii(song, legacy_song ? 8U : 14U, "MThd");
    std::vector<std::byte> sample(50);
    ascii(sample, 0, "FORM");
    be32(sample, 4, truncated ? 100U : 42U);
    ascii(sample, 8, "AIFF");
    ascii(sample, 12, "COMM");
    be32(sample, 16, 18);
    ascii(sample, 38, "APPL");
    be32(sample, 42, 4);
    const std::vector<std::pair<std::string, std::vector<std::byte>>> files{
        {generic ? "OTHER   DAT" : "SONGCONTDAT", control_fixture()},
        {"SONG    SSQ", song},
        {sample_name + std::string(8U - sample_name.size(), ' ') + "SSP", sample},
        {"README_L   ", {std::byte{'X'}}}};
    std::uint16_t cluster = 2;
    for (std::size_t index = 0; index < files.size(); ++index) {
        const auto &[name, data] = files[index];
        const auto record = 3U * 512U + index * 32U;
        ascii(bytes, record, name);
        bytes[record + 11U] = std::byte{0x20};
        le16(bytes, record + 26U, cluster);
        le32(bytes, record + 28U, static_cast<std::uint32_t>(data.size()));
        std::ranges::copy(data, bytes.begin() + 4U * 512U + (cluster - 2U) * 512U);
        const auto count = (data.size() + 511U) / 512U;
        for (std::size_t part = 0; part < count; ++part, ++cluster)
            for (const auto fat : {512U, 1024U})
                set_fat12(std::span{bytes}.subspan(fat, 512U), cluster,
                          part + 1U == count ? 0xfffU : static_cast<std::uint16_t>(cluster + 1U));
    }
    return bytes;
}
} // namespace
