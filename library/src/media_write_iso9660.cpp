#include "axklib/writer_internal.hpp"

#include "axklib/file_publication.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace axk::detail {
namespace {

constexpr std::size_t sector_size = 2048U;
constexpr std::string_view yamaha_iso_system_id = "APPLE COMPUTER, INC., TYPE: 0002";

void little16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void big16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>(value & 0xffU);
}

void both16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
    little16(bytes, offset, value);
    big16(bytes, offset + 2U, value);
}

void little32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index)
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
}

void big32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index)
        bytes[offset + index] = static_cast<std::byte>((value >> ((3U - index) * 8U)) & 0xffU);
}

void both32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    little32(bytes, offset, value);
    big32(bytes, offset + 4U, value);
}

void ascii(std::span<std::byte> bytes, std::size_t offset, std::size_t width, std::string_view value) {
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), width, std::byte{' '});
    std::ranges::transform(value.substr(0, width), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                           [](char character) { return static_cast<std::byte>(character); });
}

std::array<std::byte, 7> recording_time() {
    return {std::byte{70}, std::byte{1}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
}

std::vector<std::byte> directory_record(const Iso9660LayoutNode &node, std::span<const std::byte> name) {
    const auto length = 33U + name.size() + (name.size() % 2U == 0U ? 1U : 0U);
    std::vector<std::byte> result(length);
    result[0] = static_cast<std::byte>(length);
    little32(result, 2U, node.sector);
    big32(result, 6U, node.sector);
    little32(result, 10U, node.extent_size);
    big32(result, 14U, node.extent_size);
    const auto time = recording_time();
    std::ranges::copy(time, result.begin() + 18);
    result[25] = node.directory ? std::byte{2} : std::byte{0};
    both16(result, 28U, 1U);
    result[32] = static_cast<std::byte>(name.size());
    std::ranges::copy(name, result.begin() + 33);
    return result;
}

void append_directory_record(std::span<std::byte> directory, std::size_t &offset, std::span<const std::byte> record) {
    const auto remaining = sector_size - offset % sector_size;
    if (record.size() > remaining)
        offset += remaining;
    std::ranges::copy(record, directory.begin() + static_cast<std::ptrdiff_t>(offset));
    offset += record.size();
}

} // namespace

Result<void> write_iso9660_image(const PreparedMediaImage &image, TemporaryPublication &publication,
                                 const CancellationToken &cancellation) {
    auto planned = plan_iso9660_layout(image);
    if (!planned)
        return std::unexpected{planned.error()};
    auto &layout = *planned;
    if (layout.output_bytes > image.limits.maximum_output_bytes) {
        return std::unexpected{make_error(ErrorCode::io_unsupported_size, ErrorCategory::io,
                                          "ISO9660 output exceeds the configured build limit")};
    }
    if (auto resized = publication.resize(layout.output_bytes); !resized)
        return resized;

    if (auto written = publication.write_at(static_cast<std::uint64_t>(layout.little_path_sector) * sector_size,
                                            layout.little_path_table);
        !written) {
        return written;
    }
    if (auto written = publication.write_at(static_cast<std::uint64_t>(layout.big_path_sector) * sector_size,
                                            layout.big_path_table);
        !written) {
        return written;
    }

    const std::array<std::byte, 1> dot{std::byte{0}};
    const std::array<std::byte, 1> dotdot{std::byte{1}};
    for (const auto index : layout.directory_indices) {
        const auto &node = layout.nodes[index];
        std::vector<std::byte> directory_bytes(node.extent_size);
        std::size_t offset{};
        const auto self = directory_record(node, dot);
        const auto parent = directory_record(layout.nodes[index == 0U ? 0U : node.parent], dotdot);
        append_directory_record(directory_bytes, offset, self);
        append_directory_record(directory_bytes, offset, parent);
        for (std::size_t child_index = 1U; child_index < layout.nodes.size(); ++child_index) {
            const auto &child = layout.nodes[child_index];
            if (child.parent != index)
                continue;
            const auto record = directory_record(child, std::as_bytes(std::span{child.name}));
            append_directory_record(directory_bytes, offset, record);
        }
        if (offset > directory_bytes.size()) {
            return std::unexpected{make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction,
                                              "ISO9660 directory allocation disagrees with serialization")};
        }
        if (auto written = publication.write_at(static_cast<std::uint64_t>(node.sector) * sector_size, directory_bytes);
            !written) {
            return written;
        }
    }

    constexpr std::size_t write_chunk_size = 1024U * 1024U;
    for (const auto &node : layout.nodes) {
        if (node.directory)
            continue;
        std::uint64_t offset{};
        std::vector<std::byte> buffer(write_chunk_size);
        while (offset < node.payload_size()) {
            if (const auto check = cancellation.check(); !check)
                return std::unexpected{check.error()};
            const auto count =
                static_cast<std::size_t>(std::min<std::uint64_t>(write_chunk_size, node.payload_size() - offset));
            auto chunk = std::span{buffer}.first(count);
            if (node.external_data != nullptr) {
                if (auto read = node.external_data->read_exact_at(offset, chunk); !read)
                    return std::unexpected{read.error()};
            } else {
                std::ranges::copy(std::span{node.owned_data}.subspan(static_cast<std::size_t>(offset), count),
                                  chunk.begin());
            }
            if (auto written =
                    publication.write_at(static_cast<std::uint64_t>(node.sector) * sector_size + offset, chunk);
                !written) {
                return written;
            }
            offset += count;
        }
    }

    std::array<std::byte, sector_size> pvd_bytes{};
    auto pvd = std::span{pvd_bytes};
    pvd[0] = std::byte{1};
    ascii(pvd, 1U, 5U, "CD001");
    pvd[6] = std::byte{1};
    ascii(pvd, 8U, 32U, yamaha_iso_system_id);
    ascii(pvd, 40U, 32U, image.manifest.iso_volume_id);
    both32(pvd, 80U, layout.sector_count);
    both16(pvd, 120U, 1U);
    both16(pvd, 124U, 1U);
    both16(pvd, 128U, sector_size);
    both32(pvd, 132U, static_cast<std::uint32_t>(layout.little_path_table.size()));
    little32(pvd, 140U, layout.little_path_sector);
    big32(pvd, 148U, layout.big_path_sector);
    const auto root_record = directory_record(layout.nodes.front(), dot);
    std::ranges::copy(root_record, pvd.begin() + 156);
    ascii(pvd, 190U, 128U, image.manifest.iso_volume_id);
    ascii(pvd, 318U, 128U, "AXKLIB");
    ascii(pvd, 446U, 128U, "AXKLIB");
    ascii(pvd, 574U, 128U, "AXKLIB");
    for (const auto offset : {813U, 830U, 864U}) {
        ascii(pvd, offset, 16U, "1970010100000000");
        pvd[offset + 16U] = std::byte{0};
    }
    pvd[881] = std::byte{1};
    std::array<std::byte, sector_size> terminator_bytes{};
    auto terminator = std::span{terminator_bytes};
    terminator[0] = std::byte{255};
    ascii(terminator, 1U, 5U, "CD001");
    terminator[6] = std::byte{1};

    if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
    if (auto written = publication.write_at(16U * sector_size, pvd); !written)
        return written;
    return publication.write_at(17U * sector_size, terminator);
}

} // namespace axk::detail
