#include "fat_files_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "media_internal.hpp"

namespace axk::fat_files {
Result<void> check_path(const FilesystemPath &path) {
    if (path.empty() || path.size() > 64U ||
        std::ranges::any_of(path, [](const auto &name) { return name.size() > 12U || detail::unsafe_component(name); }))
        return std::unexpected(error("Invalid FAT edit path"));
    return {};
}
Result<void> State::reserve_directory_bytes(std::size_t bytes) {
    constexpr std::size_t limit = 64U * 1024U * 1024U;
    if (bytes > limit - directory_bytes)
        return std::unexpected(error("FAT directories exceed the aggregate byte limit"));
    directory_bytes += bytes;
    return {};
}
Result<std::array<std::byte, 11>> short_name(std::string_view name) {
    const auto uppercase = detail::upper_ascii(std::string{name});
    name = uppercase;
    const auto dot = name.find('.');
    const auto stem = name.substr(0, dot);
    const auto extension = dot == std::string_view::npos ? std::string_view{} : name.substr(dot + 1U);
    constexpr std::string_view allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'()-@^_`{}~";
    const auto valid = [&](std::string_view part) {
        return std::ranges::all_of(part, [&](char c) { return allowed.find(c) != std::string_view::npos; });
    };
    if (stem.empty() || stem.size() > 8U || extension.size() > 3U ||
        (dot != std::string_view::npos && extension.empty()) || !valid(stem) || !valid(extension))
        return std::unexpected(error("FAT names require 1-8 ASCII characters and an optional 1-3 character extension"));
    std::array<std::byte, 11> result;
    result.fill(std::byte{' '});
    for (std::size_t i = 0; i < stem.size(); ++i)
        result[i] = static_cast<std::byte>(stem[i]);
    for (std::size_t i = 0; i < extension.size(); ++i)
        result[8U + i] = static_cast<std::byte>(extension[i]);
    return result;
}

Result<std::vector<std::uint16_t>> State::allocate(std::uint32_t count) {
    std::vector<std::uint16_t> result;
    const auto limit = geometry.profile == FatProfile::fat16
                           ? std::min(geometry.backed_data_cluster_count + 2U, 0xfff0U)
                           : geometry.backed_data_cluster_count + 2U;
    for (std::uint32_t cluster = 2U; cluster < limit && result.size() < count; ++cluster) {
        if (auto check = cancellation.check(); !check)
            return std::unexpected(check.error());
        if (detail::le16(fat, static_cast<std::size_t>(cluster) * 2U) == 0U)
            result.push_back(static_cast<std::uint16_t>(cluster));
    }
    if (result.size() != count)
        return std::unexpected(
            error(geometry.backed_data_cluster_count < geometry.data_cluster_count
                      ? "FAT volume has insufficient safely usable space; its incomplete final cluster is excluded"
                      : "FAT volume has insufficient free clusters"));
    for (std::size_t i = 0; i < result.size(); ++i)
        put16(fat, static_cast<std::size_t>(result[i]) * 2U,
              i + 1U == result.size() ? std::uint16_t{0xffffU} : result[i + 1U]);
    if (!result.empty())
        fat_dirty = true;
    return result;
}
void State::release(Node &node) {
    for (const auto cluster : node.clusters)
        put16(fat, static_cast<std::size_t>(cluster) * 2U, 0U);
    if (!node.clusters.empty())
        fat_dirty = true;
    node.clusters.clear();
}
void State::store(Node &node) {
    auto &parent = nodes.at(node.parent);
    std::copy(node.entry.begin(), node.entry.end(),
              parent.directory_data.begin() + static_cast<std::ptrdiff_t>(node.slot));
    parent.dirty = true;
}
Result<std::size_t> State::slot(Node &directory, std::size_t count) {
    std::size_t run{};
    bool ended{};
    for (std::size_t offset = 0; offset < directory.directory_data.size(); offset += 32U) {
        const auto first = directory.directory_data[offset];
        ended = ended || first == std::byte{};
        run = ended || first == std::byte{0xe5} ? run + 1U : 0U;
        if (run == count) {
            if (ended && offset + 32U < directory.directory_data.size())
                directory.directory_data[offset + 32U] = std::byte{};
            return offset - (count - 1U) * 32U;
        }
    }
    if (directory.clusters.empty())
        return std::unexpected(error("FAT root directory is full"));
    if (directory.directory_data.size() + geometry.cluster_size() > 16U * 1024U * 1024U)
        return std::unexpected(error("FAT directory exceeds its byte limit"));
    if (auto reserved = reserve_directory_bytes(geometry.cluster_size()); !reserved)
        return std::unexpected(reserved.error());
    auto cluster = allocate(1U);
    if (!cluster)
        return std::unexpected(cluster.error());
    put16(fat, static_cast<std::size_t>(directory.clusters.back()) * 2U, cluster->front());
    directory.clusters.push_back(cluster->front());
    const auto offset = directory.directory_data.size();
    directory.directory_data.resize(offset + geometry.cluster_size());
    return slot(directory, count);
}

Result<void> State::remove(const std::string &path, bool recursive) {
    std::vector<std::string> removed;
    for (const auto &[key, node] : nodes) {
        if (key != path && !key.starts_with(path + '/'))
            continue;
        if ((std::to_integer<std::uint8_t>(node.entry[11]) & 0x01U) != 0U)
            return std::unexpected(error("FAT read-only entries cannot be deleted"));
        removed.push_back(key);
    }
    if (!recursive && removed.size() > 1U)
        return std::unexpected(error("Nonempty FAT directories require recursive deletion"));
    for (auto it = removed.rbegin(); it != removed.rend(); ++it) {
        if (auto check = cancellation.check(); !check)
            return check;
        auto &node = nodes.at(*it);
        auto &parent = nodes.at(node.parent);
        parent.directory_data[node.slot] = std::byte{0xe5};
        for (const auto slot : node.long_slots)
            parent.directory_data[slot] = std::byte{0xe5};
        parent.dirty = true;
        directory_bytes -= node.directory_data.size();
        release(node);
        nodes.erase(*it);
    }
    return {};
}

Result<void> State::apply(const FilesystemEdit &edit) {
    if (auto check = cancellation.check(); !check)
        return check;
    if (const auto *operation = std::get_if<MoveFilesystemEntry>(&edit))
        return move(*operation);
    return std::visit(
        [&](const auto &operation) -> Result<void> {
            using T = std::decay_t<decltype(operation)>;
            if (auto checked = check_path(operation.path); !checked)
                return checked;
            if constexpr (std::is_same_v<T, PutFilesystemFile>)
                if (operation.conflict != FileConflict::skip && operation.conflict != FileConflict::replace)
                    return std::unexpected(error("File conflict policy is invalid"));
            std::string parent_path;
            for (std::size_t i = 0; i + 1U < operation.path.size(); ++i) {
                parent_path += (parent_path.empty() ? "" : "/") + detail::upper_ascii(operation.path[i]);
                const auto parent = nodes.find(parent_path);
                if (parent == nodes.end() || !parent->second.directory)
                    return std::unexpected(error("FAT destination directory does not exist"));
            }
            const auto path =
                parent_path + (parent_path.empty() ? "" : "/") + detail::upper_ascii(operation.path.back());
            auto found = nodes.find(path);
            if constexpr (std::is_same_v<T, RemoveFilesystemEntry>) {
                if (found == nodes.end())
                    return std::unexpected(error("FAT entry to delete does not exist"));
                return remove(path, operation.recursive);
            } else if constexpr (std::is_same_v<T, RenameFilesystemEntry>) {
                if (found == nodes.end())
                    return std::unexpected(error("FAT entry to rename does not exist"));
                if ((std::to_integer<std::uint8_t>(found->second.entry[11]) & 0x01U) != 0U)
                    return std::unexpected(error("FAT read-only entries cannot be renamed"));
                auto name = short_name(operation.new_name);
                if (!name)
                    return std::unexpected(name.error());
                const auto destination =
                    parent_path + (parent_path.empty() ? "" : "/") + detail::upper_ascii(operation.new_name);
                if (nodes.contains(destination))
                    return std::unexpected(error("FAT name is unchanged or already exists"));
                auto &node = found->second;
                std::copy(name->begin(), name->end(), node.entry.begin());
                // Explicit uppercase short names no longer retain a previous long-name alias.
                node.entry[12] &= std::byte{0xe7};
                for (const auto offset : node.long_slots)
                    nodes.at(parent_path).directory_data[offset] = std::byte{0xe5};
                node.long_slots.clear();
                store(node);
                std::vector<std::string> keys;
                for (const auto &[key, value] : nodes) {
                    static_cast<void>(value);
                    if (key == path || key.starts_with(path + '/'))
                        keys.push_back(key);
                }
                for (const auto &key : keys) {
                    auto moved = nodes.extract(key);
                    moved.key() = destination + key.substr(path.size());
                    auto &parent = moved.mapped().parent;
                    if (parent == path || parent.starts_with(path + '/'))
                        parent = destination + parent.substr(path.size());
                    nodes.insert(std::move(moved));
                }
                return {};
            } else if constexpr (std::is_same_v<T, MoveFilesystemEntry>) {
                return std::unexpected(error("Move dispatch failed"));
            } else {
                constexpr bool directory = std::is_same_v<T, CreateFilesystemDirectory>;
                if (found != nodes.end()) {
                    if (found->second.directory != directory)
                        return std::unexpected(error("FAT file/directory name collision"));
                    if constexpr (directory)
                        return {};
                    else {
                        if (operation.conflict == FileConflict::skip)
                            return {};
                        if ((std::to_integer<std::uint8_t>(found->second.entry[11]) & 0x01U) != 0U)
                            return std::unexpected(error("FAT read-only files cannot be replaced"));
                    }
                }
                if constexpr (!directory) {
                    if (!operation.contents || operation.contents->size() > std::numeric_limits<std::uint32_t>::max())
                        return std::unexpected(error("FAT file input is missing or exceeds the 32-bit size field"));
                }
                if (found == nodes.end()) {
                    auto name = short_name(operation.path.back());
                    if (!name)
                        return std::unexpected(name.error());
                    if (nodes.size() >= 100000U)
                        return std::unexpected(error("FAT entry limit exceeded"));
                    auto offset = slot(nodes.at(parent_path));
                    if (!offset)
                        return std::unexpected(offset.error());
                    Node node;
                    node.directory = directory;
                    node.parent = parent_path;
                    node.slot = *offset;
                    std::copy(name->begin(), name->end(), node.entry.begin());
                    node.entry[11] = directory ? std::byte{0x10} : std::byte{0x20};
                    // Fresh entries have a deterministic valid epoch date; existing dates stay untouched.
                    for (const auto date : {16U, 18U, 24U})
                        put16(node.entry, date, 0x0021U);
                    found = nodes.emplace(path, std::move(node)).first;
                }
                auto &node = found->second;
                if constexpr (directory) {
                    if (auto reserved = reserve_directory_bytes(geometry.cluster_size()); !reserved)
                        return reserved;
                    auto cluster = allocate(1U);
                    if (!cluster)
                        return std::unexpected(cluster.error());
                    node.clusters = std::move(*cluster);
                    node.directory_data.resize(geometry.cluster_size());
                    put16(node.entry, 26U, node.clusters.front());
                    auto dot = node.entry;
                    std::fill_n(dot.begin(), 11U, std::byte{' '});
                    dot[0] = std::byte{'.'};
                    std::copy(dot.begin(), dot.end(), node.directory_data.begin());
                    dot[1] = std::byte{'.'};
                    const auto &parent = nodes.at(parent_path);
                    put16(dot, 26U, parent.clusters.empty() ? std::uint16_t{} : parent.clusters.front());
                    std::copy(dot.begin(), dot.end(), node.directory_data.begin() + 32);
                    node.dirty = true;
                } else {
                    release(node);
                    const auto size = static_cast<std::uint32_t>(operation.contents->size());
                    const auto cluster_count = static_cast<std::uint32_t>(
                        (static_cast<std::uint64_t>(size) + geometry.cluster_size() - 1U) / geometry.cluster_size());
                    auto clusters = allocate(cluster_count);
                    if (!clusters)
                        return std::unexpected(clusters.error());
                    node.clusters = std::move(*clusters);
                    put16(node.entry, 26U, node.clusters.empty() ? std::uint16_t{} : node.clusters.front());
                    put32(node.entry, 28U, size);
                    std::uint64_t consumed{};
                    for (std::size_t i = 0; i < node.clusters.size();) {
                        if (auto check = cancellation.check(); !check)
                            return check;
                        if (patches.size() >= 250000U)
                            return std::unexpected(error("FAT edit patch limit exceeded"));
                        const auto first = i++;
                        while (i < node.clusters.size() && node.clusters[i] == node.clusters[i - 1U] + 1U)
                            ++i;
                        const auto capacity = static_cast<std::uint64_t>(i - first) * geometry.cluster_size();
                        const auto byte_count = std::min<std::uint64_t>(capacity, size - consumed);
                        const auto offset = cluster_offset(node.clusters[first]);
                        patches.push_back({offset, operation.contents, consumed, byte_count});
                        if (byte_count < capacity) {
                            auto padding = std::make_shared<MemoryReader>(
                                std::vector<std::byte>(static_cast<std::size_t>(capacity - byte_count)));
                            patches.push_back({offset + byte_count, padding, 0U, padding->size()});
                        }
                        consumed += byte_count;
                    }
                }
                store(node);
                return {};
            }
        },
        edit);
}

Result<void> State::finish() {
    for (auto &[path, node] : nodes) {
        static_cast<void>(path);
        if (auto check = cancellation.check(); !check)
            return check;
        if (!node.dirty)
            continue;
        auto data = std::make_shared<MemoryReader>(std::move(node.directory_data));
        if (node.clusters.empty())
            patches.push_back({base + geometry.root_offset, data, 0U, data->size()});
        else
            for (std::size_t i = 0; i < node.clusters.size(); ++i)
                patches.push_back({cluster_offset(node.clusters[i]), data,
                                   static_cast<std::uint64_t>(i) * geometry.cluster_size(), geometry.cluster_size()});
    }
    if (fat_dirty) {
        auto data = std::make_shared<MemoryReader>(std::move(fat));
        for (std::uint8_t copy = 0U; copy < geometry.fat_count; ++copy)
            patches.push_back(
                {base + geometry.fat_offset + static_cast<std::uint64_t>(copy) * data->size(), data, 0U, data->size()});
    }
    return {};
}
} // namespace axk::fat_files
