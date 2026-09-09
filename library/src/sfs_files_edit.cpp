#include "sfs_files_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "axklib/bytes.hpp"

namespace axk::sfs_files {
Result<void> check_path(const FilesystemPath &path) {
    if (path.empty() || path.size() >= OpenOptions{}.max_directory_depth)
        return std::unexpected{error("choose an entry below the partition root")};
    for (const auto &component : path)
        if (component.empty() || component.size() > 23U || component == "." || component == ".." ||
            std::ranges::any_of(component,
                                [](unsigned char c) { return c < 0x20U || c > 0x7eU || c == '/' || c == '\\'; }))
            return std::unexpected{
                error("SFS entry names require 1 to 23 printable ASCII bytes without path separators")};
    if (is_partition_support_root_entry(path.front()))
        return std::unexpected{error("filesystem metadata is protected from raw edits")};
    return {};
}

namespace {
Result<std::optional<std::size_t>> find_entry(const Record &directory, std::string_view name) {
    std::optional<std::size_t> found;
    for (std::size_t offset = 0; offset + 32U <= directory.directory.size(); offset += 32U) {
        const ByteReader reader{std::span{directory.directory}.subspan(offset, 32U)};
        if (reader.be16(0).value() == 0U)
            break;
        if (directory_entry_state(LinkId{reader.be32(4).value()}) != DirectoryEntryState::live)
            continue;
        const auto size = reader.be16(2).value();
        const auto entry_name = reader.ascii_field(8U, size, false);
        if (!entry_name)
            return std::unexpected{entry_name.error()};
        if (*entry_name != name)
            continue;
        if (found)
            return std::unexpected{error("filesystem path has duplicate names")};
        found = offset;
    }
    return found;
}

Result<Record *> child(State &state, const Record &parent, std::size_t offset) {
    const auto link = ByteReader{parent.directory}.be32(offset + 4U);
    if (!link)
        return std::unexpected{link.error()};
    const auto found = state.records.find(*link);
    if (found == state.records.end() || found->second.deleted)
        return std::unexpected{error("filesystem entry target is missing")};
    return &found->second;
}

Result<void> entry_bytes(std::span<std::byte> destination, std::string_view name, SfsId id) {
    std::ranges::fill(destination, std::byte{});
    ByteWriter writer{destination};
    if (auto written = writer.write_be16(0U, 32U); !written)
        return written;
    if (auto written = writer.write_be16(2U, static_cast<std::uint16_t>(name.size() + 1U)); !written)
        return written;
    if (auto written = writer.write_be32(4U, id.value); !written)
        return written;
    for (std::size_t i = 0; i < name.size(); ++i)
        destination[8U + i] = static_cast<std::byte>(name[i]);
    return {};
}

Result<void> insert(Record &parent, std::string_view name, SfsId id) {
    std::size_t offset{};
    for (; offset + 32U <= parent.directory.size(); offset += 32U) {
        const ByteReader reader{parent.directory};
        if (reader.be16(offset).value() == 0U ||
            directory_entry_state(LinkId{reader.be32(offset + 4U).value()}) == DirectoryEntryState::deleted)
            break;
    }
    if (offset == parent.directory.size()) {
        if (parent.directory.size() >= OpenOptions{}.max_directory_bytes)
            return std::unexpected{error("directory exceeds the supported traversal size")};
        parent.directory.resize(offset + 32U);
    }
    parent.changed = true;
    parent.directory_changed = true;
    return entry_bytes(std::span{parent.directory}.subspan(offset, 32U), name, id);
}
} // namespace

Result<void> State::remove(Record &parent, std::size_t offset, bool recursive) {
    if (auto checked = cancellation.check(); !checked)
        return checked;
    auto target = child(*this, parent, offset);
    if (!target)
        return std::unexpected{target.error()};
    auto &record = **target;
    if (protected_records.contains(record.info.sfs_id.value))
        return std::unexpected{error("filesystem metadata is protected from deletion")};
    if (record.info.payload_kind == PayloadKind::directory) {
        if (auto loaded = load_directory(record); !loaded)
            return loaded;
        // Directory aliases would make recursive removal affect unselected trees.
        const auto parent_id = ByteReader{record.directory}.be32(36U).value();
        if (parent_id != parent.info.sfs_id.value)
            return std::unexpected{error("directory parent does not match the selected path")};
        for (std::size_t entry = 64U; entry + 32U <= record.directory.size(); entry += 32U) {
            const ByteReader reader{record.directory};
            if (reader.be16(entry).value() == 0U)
                break;
            if (directory_entry_state(LinkId{reader.be32(entry + 4U).value()}) != DirectoryEntryState::live)
                continue;
            if (!recursive)
                return std::unexpected{error("recursive deletion must be explicitly confirmed")};
            if (auto removed = remove(record, entry, true); !removed)
                return removed;
        }
        if (record.info.link_count != 2U)
            return std::unexpected{error("directory still has references outside the selected tree")};
        if (auto changed = change_links(parent, -1); !changed)
            return changed;
        if (auto changed = change_links(record, -2); !changed)
            return changed;
    } else {
        if (auto changed = change_links(record, -1); !changed)
            return changed;
        if (record.info.link_count == 1U)
            record.info.attributes &= 0xfe000000U;
    }
    if (record.info.link_count == 0U) {
        if (auto freed = release(record); !freed)
            return freed;
        record.deleted = true;
        std::vector<std::byte>{}.swap(record.directory);
        occupied_slots.erase(record.info.sfs_id.value);
    }
    parent.changed = true;
    parent.directory_changed = true;
    return ByteWriter{parent.directory}.write_be32(offset + 4U, 0xf0000000U | record.info.sfs_id.value);
}

Result<void> State::apply(const FilesystemEdit &edit) {
    return std::visit(
        [&](const auto &operation) -> Result<void> {
            using T = std::decay_t<decltype(operation)>;
            if (auto checked = cancellation.check(); !checked)
                return checked;
            if (auto checked = check_path(operation.path); !checked)
                return checked;
            auto *parent = &records.at(root.value);
            if (auto loaded = load_directory(*parent); !loaded)
                return loaded;
            for (std::size_t i = 0; i + 1U < operation.path.size(); ++i) {
                auto entry = find_entry(*parent, operation.path[i]);
                if (!entry)
                    return std::unexpected{entry.error()};
                if (!*entry)
                    return std::unexpected{error("filesystem destination directory does not exist")};
                auto next = child(*this, *parent, **entry);
                if (!next)
                    return std::unexpected{next.error()};
                if ((*next)->info.payload_kind != PayloadKind::directory)
                    return std::unexpected{error("filesystem path contains a file instead of a directory")};
                parent = *next;
                if (protected_records.contains(parent->info.sfs_id.value))
                    return std::unexpected{error("filesystem metadata is protected from raw edits")};
                if (auto loaded = load_directory(*parent); !loaded)
                    return loaded;
            }
            const auto &name = operation.path.back();
            auto found = find_entry(*parent, name);
            if (!found)
                return std::unexpected{found.error()};
            Record *existing{};
            if (*found) {
                auto selected = child(*this, *parent, **found);
                if (!selected)
                    return std::unexpected{selected.error()};
                existing = *selected;
                if (protected_records.contains(existing->info.sfs_id.value))
                    return std::unexpected{error("filesystem metadata is protected from raw edits")};
            }
            if constexpr (std::is_same_v<T, RemoveFilesystemEntry>) {
                if (!*found)
                    return std::unexpected{error("filesystem entry to delete does not exist")};
                return remove(*parent, **found, operation.recursive);
            } else if constexpr (std::is_same_v<T, CreateFilesystemDirectory>) {
                if (existing)
                    return existing->info.payload_kind == PayloadKind::directory
                               ? Result<void>{}
                               : std::unexpected{error("a file already occupies the directory name")};
                auto created = create(true);
                if (!created)
                    return std::unexpected{created.error()};
                auto &directory = **created;
                directory.directory.resize(64U);
                if (auto written = entry_bytes(std::span{directory.directory}.first(32U), ".", directory.info.sfs_id);
                    !written)
                    return written;
                if (auto written = entry_bytes(std::span{directory.directory}.subspan(32U), "..", parent->info.sfs_id);
                    !written)
                    return written;
                directory.directory_changed = true;
                if (auto changed = change_links(*parent, 1); !changed)
                    return changed;
                return insert(*parent, name, directory.info.sfs_id);
            } else {
                if (operation.conflict != FileConflict::skip && operation.conflict != FileConflict::replace)
                    return std::unexpected{error("file conflict policy is invalid")};
                if (!operation.contents)
                    return std::unexpected{error("file contents are required")};
                if (existing && existing->info.payload_kind == PayloadKind::directory)
                    return std::unexpected{error("a directory already occupies the file name")};
                if (existing && operation.conflict == FileConflict::skip)
                    return {};
                if (existing && existing->info.link_count == 1U)
                    return replace_payload(*existing, operation.contents);
                std::optional<std::array<std::byte, 72>> previous;
                if (existing) {
                    previous = existing->raw;
                    if (auto removed = remove(*parent, **found, false); !removed)
                        return removed;
                }
                auto created = create(false);
                if (!created)
                    return std::unexpected{created.error()};
                if (previous) {
                    (*created)->raw = *previous;
                    (*created)->info.attributes = ByteReader{*previous}.be32(0x42U).value() & 0xfe000000U;
                }
                if (auto written = replace_payload(**created, operation.contents); !written)
                    return written;
                return insert(*parent, name, (*created)->info.sfs_id);
            }
        },
        edit);
}
} // namespace axk::sfs_files
