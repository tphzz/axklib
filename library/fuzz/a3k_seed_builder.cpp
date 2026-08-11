#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

void ascii(std::span<std::byte> bytes, std::size_t offset, std::string_view value) {
    for (std::size_t index = 0U; index < value.size(); ++index)
        bytes[offset + index] = static_cast<std::byte>(value[index]);
}

void le32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index)
        bytes[offset + index] = static_cast<std::byte>(value >> (index * 8U));
}

void be16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value >> 8U);
    bytes[offset + 1U] = static_cast<std::byte>(value);
}

void be32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index)
        bytes[offset + index] = static_cast<std::byte>(value >> ((3U - index) * 8U));
}

void index_record(std::span<std::byte> record, std::string_view path, std::uint32_t offset, std::uint32_t size,
                  bool banner) {
    ascii(record, 0U, path);
    record[256U] = banner ? std::byte{} : std::byte{1U};
    le32(record, 258U, offset);
    le32(record, 262U, size);
    record[266U] = std::byte{1U};
}

std::vector<std::byte> valid_archive() {
    constexpr std::size_t header_size = 1110U;
    constexpr std::size_t object_size = 0xb0U;
    constexpr std::size_t record_size = 271U;
    const std::string banner = "\r\n       Volume Name : Fuzz Volume\r\n";
    const auto object_offset = header_size + banner.size();
    const auto index_offset = object_offset + object_size;
    std::vector<std::byte> bytes(index_offset + 2U * record_size);

    le32(bytes, 0U, 1U);
    le32(bytes, 4U, static_cast<std::uint32_t>(index_offset));
    le32(bytes, 8U, 2U);
    ascii(bytes, 12U,
          "A3k"
          "Dis"
          "kyPC");
    ascii(bytes, 70U, "XXXXXXXXXXXXXXXX");
    ascii(bytes, header_size, banner);

    auto object = std::span{bytes}.subspan(object_offset, object_size);
    ascii(object, 0U, "FSFSDEV3SPLXSMPL");
    be32(object, 0x10U, 0xacU);
    be32(object, 0x1cU, 4U);
    be32(object, 0x20U, 4U);
    be16(object, 0x28U, 32'000U);
    be16(object, 0x2aU, 2U);
    ascii(object, 0x32U, "FUZZ");
    be16(object, 0x8cU, 32'000U);
    be32(object, 0x96U, 2U);
    be32(object, 0x9eU, 2U);

    auto index = std::span{bytes}.subspan(index_offset);
    index_record(index.first(record_size), "/A3kFileInfo.txt", static_cast<std::uint32_t>(header_size),
                 static_cast<std::uint32_t>(banner.size()), true);
    index_record(index.subspan(record_size, record_size),
                 "Fuzz Volume "
                 "\\"
                 "SMPL"
                 "\\"
                 "FUZZ",
                 static_cast<std::uint32_t>(object_offset), object_size, false);
    return bytes;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: axk_a3k_seed_builder OUTPUT.a3k\n";
        return 2;
    }
    const auto bytes = valid_archive();
    std::ofstream output{std::filesystem::path{argv[1]}, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        std::cerr << "failed to write A3K archive fuzz seed\n";
        return 1;
    }
    return 0;
}
