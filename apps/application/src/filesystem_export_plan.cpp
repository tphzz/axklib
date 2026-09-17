#include "filesystem_export_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace axk::app::detail {
namespace {
std::string folded(std::string value) {
    for (auto &ch : value)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch + ('a' - 'A'));
    return value;
}

std::string export_name(std::string value) {
    for (auto &ch : value)
        if (static_cast<unsigned char>(ch) < 32U || ch == '\x7f' || std::string_view{"<>:\"/\\|?*"}.contains(ch))
            ch = '_';
    while (!value.empty() && (value.back() == '.' || value.back() == ' '))
        value.pop_back();
    if (value.empty() || value == "." || value == "..")
        value = "unnamed";
    const auto base = folded(value.substr(0U, value.find('.')));
    if (base == "con" || base == "prn" || base == "aux" || base == "nul" ||
        (base.size() == 4U && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '1' && base[3] <= '9'))
        value.insert(value.begin(), '_');
    return value;
}
} // namespace

Result<FilesystemExportPlan> plan_filesystem_export(const MediaContainer &media, std::span<const std::string> entry_ids,
                                                    const CancellationToken &cancellation,
                                                    FilesystemExportLayout layout) {
    if (entry_ids.empty() || entry_ids.size() > 10000U)
        return std::unexpected(Error{"invalid_request", "Choose between 1 and 10000 filesystem entries"});
    if (std::ranges::any_of(entry_ids, [](const auto &id) { return id.empty() || id.size() > 512U; }))
        return std::unexpected(Error{"invalid_request", "Filesystem entry identity exceeds the supported bounds"});
    if (auto checked = cancellation.check(); !checked)
        return std::unexpected(Error{"operation_cancelled", checked.error().message});
    auto built = build_image_filesystem(media, {}, {}, {}, {});
    if (!built)
        return std::unexpected(built.error());
    if (!built->available)
        return std::unexpected(Error{"unsupported_operation", "This image has no filesystem view"});
    std::map<std::string, const ImageFilesystemEntry *> entries;
    std::map<std::string, std::vector<const ImageFilesystemEntry *>> children;
    for (const auto &entry : built->entries) {
        if (!entries.emplace(entry.id, &entry).second)
            return std::unexpected(Error{"filesystem_entry_ambiguous", "Filesystem entry identities are ambiguous"});
        if (entry.parent_id)
            children[*entry.parent_id].push_back(&entry);
    }
    const std::set<std::string> selected{entry_ids.begin(), entry_ids.end()};
    for (const auto &id : selected) {
        const auto found = entries.find(id);
        if (id.empty() || id.size() > 512U || found == entries.end())
            return std::unexpected(Error{"filesystem_entry_not_found", "Filesystem entry does not exist"});
        if (found->second->filesystem_metadata)
            return std::unexpected(
                Error{"filesystem_entry_protected", "Structural metadata is not an exportable user file"});
    }
    struct Pending {
        const ImageFilesystemEntry *entry;
        std::vector<std::string> parent_path;
    };
    std::vector<Pending> pending;
    for (const auto &id : selected) {
        const auto *entry = entries.at(id);
        if (std::ranges::none_of(entry->ancestor_ids,
                                 [&](const auto &ancestor) { return selected.contains(ancestor); }))
            pending.push_back({entry, {}});
    }
    FilesystemExportPlan result;
    const auto *root =
        layout == FilesystemExportLayout::export_folder && pending.size() == 1U && pending.front().entry->kind != "file"
            ? pending.front().entry
            : nullptr;
    std::set<std::vector<std::string>> paths;
    std::size_t metadata_bytes{};
    while (!pending.empty()) {
        if (auto checked = cancellation.check(); !checked)
            return std::unexpected(Error{"operation_cancelled", checked.error().message});
        auto current = std::move(pending.back());
        pending.pop_back();
        const auto &entry = *current.entry;
        if (entry.filesystem_metadata) {
            result.summary.notices.push_back({entry.id, entry.path, "Structural filesystem metadata omitted"});
            continue;
        }
        if (!entry.issue.empty())
            return std::unexpected(Error{"filesystem_entry_unresolved", entry.path + ": " + entry.issue});
        const auto name = export_name(entry.name);
        if (name != entry.name)
            result.summary.notices.push_back({entry.id, entry.path, "Host entry name: " + name});
        if (&entry == root) {
            result.summary.root_directory = FilesystemExportRoot{entry.id, entry.path, name};
            for (const auto *child : children[entry.id])
                pending.push_back({child, {}});
            continue;
        }
        auto path = std::move(current.parent_path);
        path.push_back(name);
        auto key = path;
        for (auto &component : key)
            component = folded(std::move(component));
        if (!paths.insert(std::move(key)).second)
            return std::unexpected(
                Error{"filesystem_export_collision", "Selected entries produce conflicting host paths"});
        const bool directory = entry.kind != "file";
        if (!directory) {
            const auto locator = built->files.find(entry.id);
            if (!entry.size_bytes || locator == built->files.end())
                return std::unexpected(
                    Error{"filesystem_entry_unresolved", entry.path + ": File bytes cannot be resolved"});
            if (*entry.size_bytes > std::numeric_limits<std::uint64_t>::max() - result.summary.total_bytes)
                return std::unexpected(
                    Error{"filesystem_export_limit", "Export byte count exceeds the supported range"});
            result.summary.total_bytes += *entry.size_bytes;
            result.files.emplace(entry.id, locator->second);
        }
        metadata_bytes += sizeof(FilesystemExportEntry) + entry.id.size() + entry.path.size() + 256U;
        for (const auto &component : path)
            metadata_bytes += sizeof(std::string) + component.size();
        if (metadata_bytes > 64U * 1024U * 1024U)
            return std::unexpected(Error{"filesystem_export_limit", "Export metadata exceeds the 64 MiB budget"});
        result.summary.entries.push_back({entry.id, entry.path, path, directory, entry.size_bytes.value_or(0U)});
        if (directory)
            for (const auto *child : children[entry.id])
                pending.push_back({child, path});
    }
    std::ranges::sort(result.summary.entries, {}, &FilesystemExportEntry::relative_path);
    return result;
}
} // namespace axk::app::detail
