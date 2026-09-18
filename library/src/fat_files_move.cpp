#include "fat_files_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "media_internal.hpp"

namespace axk::fat_files {
Result<void> State::move(const MoveFilesystemEntry &operation) {
    if (auto checked = check_path(operation.path); !checked)
        return checked;
    auto full_target = operation.destination_parent;
    full_target.push_back(operation.path.back());
    if (auto checked = check_path(full_target); !checked)
        return checked;
    const auto joined = [](const FilesystemPath &parts) {
        std::string result;
        for (const auto &part : parts)
            result += (result.empty() ? "" : "/") + detail::upper_ascii(part);
        return result;
    };
    const auto path = joined(operation.path);
    const auto target = joined(operation.destination_parent);
    const auto destination = joined(full_target);
    auto found = nodes.find(path);
    auto parent = nodes.find(target);
    if (found == nodes.end() || parent == nodes.end() || !parent->second.directory)
        return std::unexpected(error("Move source or destination directory does not exist"));
    if (target == path || target.starts_with(path + '/'))
        return std::unexpected(error("A directory cannot move into itself or its descendants"));
    auto &node = found->second;
    if ((std::to_integer<unsigned>(node.entry[11]) & 1U) != 0U)
        return std::unexpected(error("FAT read-only entries cannot be moved"));
    if (node.parent == target)
        return {};
    if (nodes.contains(destination))
        return std::unexpected(error("A destination entry already has this name: " + operation.path.back()));
    auto &old_parent = nodes.at(node.parent);
    auto &new_parent = parent->second;
    const auto allocated = slot(new_parent, node.long_slots.size() + 1U);
    if (!allocated)
        return std::unexpected(allocated.error());
    auto next = *allocated;
    for (auto &old_slot : node.long_slots) {
        std::copy_n(old_parent.directory_data.begin() + static_cast<std::ptrdiff_t>(old_slot), 32U,
                    new_parent.directory_data.begin() + static_cast<std::ptrdiff_t>(next));
        old_parent.directory_data[old_slot] = std::byte{0xe5};
        old_slot = next;
        next += 32U;
    }
    old_parent.directory_data[node.slot] = std::byte{0xe5};
    old_parent.dirty = true;
    node.slot = next;
    node.parent = target;
    store(node);
    if (node.directory) {
        bool updated{};
        for (std::size_t offset = 0; offset + 32U <= node.directory_data.size(); offset += 32U) {
            const auto row = std::span{node.directory_data}.subspan(offset, 32U);
            if (row[0] != std::byte{'.'} || row[1] != std::byte{'.'} || row[2] != std::byte{' '})
                continue;
            put16(row, 26U, new_parent.clusters.empty() ? std::uint16_t{} : new_parent.clusters.front());
            updated = true;
        }
        if (!updated)
            return std::unexpected(error("Moved FAT directory has no parent entry"));
        node.dirty = true;
    }
    std::vector<std::string> keys;
    for (const auto &[key, value] : nodes) {
        static_cast<void>(value);
        if (key == path || key.starts_with(path + '/'))
            keys.push_back(key);
    }
    for (const auto &key : keys) {
        auto moved = nodes.extract(key);
        moved.key() = destination + key.substr(path.size());
        auto &parent_path = moved.mapped().parent;
        if (parent_path == path || parent_path.starts_with(path + '/'))
            parent_path = destination + parent_path.substr(path.size());
        nodes.insert(std::move(moved));
    }
    return {};
}
} // namespace axk::fat_files
