#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "image_filesystem_internal.hpp"

namespace axk::app::detail {
struct ResolvedFilesystemEntry {
    PartitionIndex partition;
    FilesystemPath path;
    const ImageFilesystemEntry *entry{};
};

// The index must outlive this resolver and its returned entry pointers.
class FilesystemEntryPaths {
  public:
    explicit FilesystemEntryPaths(const ImageFilesystemIndex &index) : index_(index) {
        entries_.reserve(index.entries.size());
        for (const auto &entry : index.entries)
            entries_.emplace(entry.id, &entry);
    }

    Result<ResolvedFilesystemEntry> resolve(std::string_view id) const {
        const auto found = entries_.find(id);
        if (id.empty() || id.size() > 512U || found == entries_.end())
            return std::unexpected(Error{"filesystem_entry_not_found", "Filesystem entry does not exist"});
        const auto &entry = *found->second;
        const auto partition = index_.edit_partitions.find(entry.root_id);
        if (partition == index_.edit_partitions.end())
            return std::unexpected(
                Error{"unsupported_operation", "Raw editing is unavailable for this filesystem root"});
        FilesystemPath path;
        const auto append = [&](const ImageFilesystemEntry &component) -> Result<void> {
            if (component.filesystem_metadata)
                return std::unexpected(Error{"filesystem_entry_protected", "Filesystem metadata is protected"});
            if (!component.issue.empty())
                return std::unexpected(Error{"filesystem_entry_unresolved", component.issue});
            if (component.parent_id)
                path.push_back(component.name);
            return {};
        };
        for (const auto &ancestor : entry.ancestor_ids) {
            const auto parent = entries_.find(ancestor);
            if (parent == entries_.end())
                return std::unexpected(Error{"filesystem_entry_unresolved", "Filesystem ancestry is incomplete"});
            if (auto checked = append(*parent->second); !checked)
                return std::unexpected(checked.error());
        }
        if (auto checked = append(entry); !checked)
            return std::unexpected(checked.error());
        return ResolvedFilesystemEntry{partition->second, std::move(path), &entry};
    }

  private:
    const ImageFilesystemIndex &index_;
    std::unordered_map<std::string_view, const ImageFilesystemEntry *> entries_;
};
} // namespace axk::app::detail
