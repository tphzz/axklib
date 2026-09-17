#include "fat_files_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "media_internal.hpp"

namespace axk::fat_files {
Error error(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
}
void put16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>(value >> 8U);
}
void put32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    put16(bytes, offset, static_cast<std::uint16_t>(value & 0xffffU));
    put16(bytes, offset + 2U, static_cast<std::uint16_t>(value >> 16U));
}
std::uint64_t State::cluster_offset(std::uint16_t cluster) const {
    return base + geometry.data_offset + static_cast<std::uint64_t>(cluster - 2U) * geometry.cluster_size();
}
namespace {
bool dot_name(std::span<const std::byte> entry, bool parent) {
    const auto length = parent ? 2U : 1U;
    return entry[0] == std::byte{'.'} && (!parent || entry[1] == std::byte{'.'}) &&
           std::ranges::all_of(entry.subspan(length, 11U - length),
                               [](std::byte byte) { return byte == std::byte{' '}; });
}
std::string parent_path(const std::string &path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}
Result<void> link_directory(State &state, Node &directory, const std::map<std::uint64_t, Node *> &offsets) {
    std::vector<std::size_t> long_slots;
    std::uint8_t remaining{};
    std::uint8_t checksum{};
    for (std::size_t slot = 0; slot < directory.directory_data.size(); slot += 32U) {
        if (auto check = state.cancellation.check(); !check)
            return check;
        const auto entry = std::span{directory.directory_data}.subspan(slot, 32U);
        const auto first = std::to_integer<std::uint8_t>(entry[0]);
        if (first == 0U)
            break;
        if (first == 0xe5U) {
            if (!long_slots.empty())
                return std::unexpected(error("FAT has orphan long-name records"));
            continue;
        }
        const auto attributes = std::to_integer<std::uint8_t>(entry[11]);
        if (attributes == 0x0fU) {
            const auto ordinal = static_cast<std::uint8_t>(first & 0x1fU);
            if (long_slots.empty()) {
                remaining = ordinal;
                checksum = std::to_integer<std::uint8_t>(entry[13]);
            }
            if (ordinal == 0U || ordinal > 20U || ordinal != remaining ||
                first != static_cast<std::uint8_t>(ordinal | (long_slots.empty() ? 0x40U : 0U)) ||
                entry[12] != std::byte{} || detail::le16(entry, 26U) != 0U ||
                std::to_integer<std::uint8_t>(entry[13]) != checksum)
                return std::unexpected(error("FAT long-name sequence is invalid"));
            --remaining;
            long_slots.push_back(slot);
            continue;
        }
        if (!long_slots.empty()) {
            std::uint8_t actual{};
            for (const auto byte : entry.first(11U))
                actual = static_cast<std::uint8_t>(((actual & 1U) << 7U) + (actual >> 1U) +
                                                   std::to_integer<std::uint8_t>(byte));
            if (remaining != 0U || actual != checksum || (attributes & 0x08U) != 0U || dot_name(entry, false) ||
                dot_name(entry, true))
                return std::unexpected(error("FAT long-name records do not match their short entry"));
        }
        if (detail::le16(entry, 20U) != 0U)
            return std::unexpected(error("FAT16 entry has a nonzero high cluster field"));
        if (dot_name(entry, false) || dot_name(entry, true)) {
            const auto &target = dot_name(entry, true) ? state.nodes.at(directory.parent) : directory;
            const auto expected = target.clusters.empty() ? std::uint16_t{} : target.clusters.front();
            if (directory.clusters.empty() || (attributes & 0x18U) != 0x10U || detail::le32(entry, 28U) != 0U ||
                detail::le16(entry, 26U) != expected)
                return std::unexpected(error("FAT directory self/parent entry is invalid"));
            continue;
        }
        if ((attributes & 0x08U) != 0U)
            continue;
        const auto physical = directory.clusters.empty()
                                  ? state.base + state.geometry.root_offset + slot
                                  : state.cluster_offset(directory.clusters[slot / state.geometry.cluster_size()]) +
                                        slot % state.geometry.cluster_size();
        const auto found = offsets.find(physical);
        if (found == offsets.end())
            return std::unexpected(error("FAT directory entry is not in the validated inventory"));
        auto &node = *found->second;
        if (node.directory && detail::le32(entry, 28U) != 0U)
            return std::unexpected(error("FAT directory has a nonzero file size"));
        node.slot = slot;
        std::copy(entry.begin(), entry.end(), node.entry.begin());
        node.long_slots = std::move(long_slots);
        long_slots.clear();
    }
    if (!long_slots.empty())
        return std::unexpected(error("FAT has orphan long-name records"));
    return {};
}
} // namespace

Result<State> open(std::shared_ptr<const RandomAccessReader> source, PartitionIndex partition,
                   const CancellationToken &cancellation) {
    if (!source)
        return std::unexpected(error("FAT source is required"));
    auto media = open_media(source, {}, cancellation);
    if (!media)
        return std::unexpected(media.error());
    const auto *image = std::get_if<FatImage>(&media->storage());
    State state;
    state.source = source;
    state.cancellation = cancellation;
    state.region_size = source->size();
    if (const auto *disk = std::get_if<FatDiskImage>(&media->storage())) {
        if (partition.value >= disk->partitions().size())
            return std::unexpected(error("FAT partition does not exist"));
        const auto &selected = disk->partitions()[partition.value];
        image = &selected.volume;
        state.base = selected.byte_offset;
        state.region_size = selected.size_bytes;
    } else if (partition.value != 0U)
        return std::unexpected(error("FAT volume has no such partition"));
    if (!image || image->geometry().profile == FatProfile::a_series_floppy)
        return std::unexpected(error("Raw FAT editing requires a supported FAT16 profile"));
    state.geometry = image->geometry();
    const auto &g = state.geometry;
    auto fat = detail::read_bytes(*source, state.base + g.fat_offset,
                                  static_cast<std::size_t>(g.sectors_per_fat) * g.bytes_per_sector, cancellation);
    if (!fat)
        return std::unexpected(fat.error());
    if (detail::le16(*fat, 0U) != static_cast<std::uint16_t>(0xff00U | g.media_descriptor) ||
        detail::le16(*fat, 2U) != 0xffffU)
        return std::unexpected(error("FAT reserved entries or clean-volume flags are invalid for editing"));
    state.fat = std::move(*fat);
    auto root = detail::read_bytes(*source, state.base + g.root_offset,
                                   static_cast<std::size_t>(g.root_entry_count) * 32U, cancellation);
    if (!root)
        return std::unexpected(root.error());
    Node root_node;
    root_node.directory = true;
    root_node.directory_data = std::move(*root);
    if (auto reserved = state.reserve_directory_bytes(root_node.directory_data.size()); !reserved)
        return std::unexpected(reserved.error());
    state.nodes.emplace("", std::move(root_node));
    for (const auto &directory : image->directories()) {
        Node node;
        node.directory = true;
        node.parent = detail::upper_ascii(parent_path(directory.path));
        node.original_offset = state.base + directory.directory_offset;
        node.clusters = directory.clusters;
        if (auto reserved = state.reserve_directory_bytes(node.clusters.size() * g.cluster_size()); !reserved)
            return std::unexpected(reserved.error());
        node.directory_data.reserve(node.clusters.size() * g.cluster_size());
        for (const auto cluster : node.clusters) {
            auto block = detail::read_bytes(*source, state.cluster_offset(cluster), g.cluster_size(), cancellation);
            if (!block)
                return std::unexpected(block.error());
            node.directory_data.insert(node.directory_data.end(), block->begin(), block->end());
        }
        state.nodes.emplace(detail::upper_ascii(directory.path), std::move(node));
    }
    for (const auto &file : image->files()) {
        Node node;
        node.parent = detail::upper_ascii(parent_path(file.path));
        node.original_offset = state.base + file.directory_offset;
        node.clusters = file.clusters;
        state.nodes.emplace(detail::upper_ascii(file.path), std::move(node));
    }
    std::map<std::uint64_t, Node *> offsets;
    for (auto &[path, node] : state.nodes)
        if (!path.empty())
            offsets.emplace(node.original_offset, &node);
    for (auto &[path, node] : state.nodes) {
        static_cast<void>(path);
        if (node.directory)
            if (auto checked = link_directory(state, node, offsets); !checked)
                return std::unexpected(checked.error());
    }
    return state;
}
} // namespace axk::fat_files
