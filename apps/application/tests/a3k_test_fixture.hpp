#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/media.hpp"

namespace axk::app::test {

inline void write_le32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    ASSERT_LE(offset + 4U, bytes.size());
    bytes[offset] = static_cast<std::byte>(value);
    bytes[offset + 1U] = static_cast<std::byte>(value >> 8U);
    bytes[offset + 2U] = static_cast<std::byte>(value >> 16U);
    bytes[offset + 3U] = static_cast<std::byte>(value >> 24U);
}

inline void write_ascii(std::span<std::byte> bytes, std::size_t offset, std::string_view value) {
    ASSERT_LE(offset + value.size(), bytes.size());
    std::ranges::transform(value, bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                           [](char character) { return static_cast<std::byte>(character); });
}

inline void write_a3k_index_record(std::span<std::byte> record, std::string_view path, std::uint32_t offset,
                                   std::uint32_t size, bool banner) {
    ASSERT_EQ(record.size(), 271U);
    ASSERT_LT(path.size(), 256U);
    write_ascii(record, 0U, path);
    record[256U] = banner ? std::byte{} : std::byte{1U};
    write_le32(record, 258U, offset);
    write_le32(record, 262U, size);
    record[266U] = std::byte{1U};
}

inline void write_a3k_archive(const std::filesystem::path &source, const std::filesystem::path &destination,
                              std::string_view volume_name = "Archive Volume") {
    constexpr std::size_t header_size = 1110U;
    constexpr std::size_t record_size = 271U;
    auto media = axk::open_media(source);
    ASSERT_TRUE(media) << media.error().message;
    auto objects = media->objects(axk::MediaObjectReadMode::complete);
    ASSERT_TRUE(objects) << objects.error().message;
    ASSERT_FALSE(objects->empty());

    const auto banner = std::string{"\r\n       Volume Name : "} + std::string{volume_name} + "\r\n";
    std::size_t index_offset = header_size + banner.size();
    for (const auto &object : *objects)
        index_offset += object.raw_payload.size();
    ASSERT_LE(index_offset, std::numeric_limits<std::uint32_t>::max());
    ASSERT_LE(objects->size() + 1U, std::numeric_limits<std::uint32_t>::max());

    std::vector<std::byte> bytes(index_offset + (objects->size() + 1U) * record_size);
    write_le32(bytes, 0U, 1U);
    write_le32(bytes, 4U, static_cast<std::uint32_t>(index_offset));
    write_le32(bytes, 8U, static_cast<std::uint32_t>(objects->size() + 1U));
    write_ascii(bytes, 12U,
                "A3k"
                "Dis"
                "kyPC");
    write_ascii(bytes, 70U, "XXXXXXXXXXXXXXXX");
    write_ascii(bytes, header_size, banner);

    auto index = std::span{bytes}.subspan(index_offset);
    write_a3k_index_record(index.first(record_size), "/A3kFileInfo.txt", static_cast<std::uint32_t>(header_size),
                           static_cast<std::uint32_t>(banner.size()), true);
    std::size_t payload_offset = header_size + banner.size();
    for (std::size_t object_index = 0U; object_index < objects->size(); ++object_index) {
        const auto &object = (*objects)[object_index];
        std::ranges::copy(object.raw_payload, bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset));
        const auto path =
            std::string{volume_name} + " \\" + object.decoded.header.raw_type + "\\" + object.decoded.header.name;
        write_a3k_index_record(index.subspan((object_index + 1U) * record_size, record_size), path,
                               static_cast<std::uint32_t>(payload_offset),
                               static_cast<std::uint32_t>(object.raw_payload.size()), false);
        payload_offset += object.raw_payload.size();
    }

    std::ofstream output{destination, std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(output);
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output);
}

} // namespace axk::app::test
