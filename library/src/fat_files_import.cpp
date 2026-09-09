#include "axklib/filesystem_import.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "fat_files_internal.hpp"
#include "media_internal.hpp"

namespace axk {
namespace {
using Action = FilesystemImportAction;
struct ReviewNode {
    bool directory{};
    bool readonly{};
    bool incoming{};
    std::uint64_t size{};
};
class ImportReview {
  public:
    explicit ImportReview(fat_files::State state) : cancellation_(state.cancellation) {
        for (const auto &[path, node] : state.nodes)
            nodes_.emplace(path, ReviewNode{node.directory, (std::to_integer<std::uint8_t>(node.entry[11]) & 1U) != 0U,
                                            false, detail::le32(node.entry, 28U)});
    }

    Result<std::vector<FilesystemImportDecision>> run(std::span<const FilesystemImportEntry> entries) {
        std::vector<FilesystemImportDecision> result;
        result.reserve(entries.size());
        for (const auto &entry : entries) {
            if (auto checked = cancellation_.check(); !checked)
                return std::unexpected(checked.error());
            const auto decision = inspect(entry);
            result.push_back(decision ? *decision
                                      : FilesystemImportDecision{Action::conflict, {}, decision.error().message});
        }
        return result;
    }

  private:
    Result<FilesystemImportDecision> inspect(const FilesystemImportEntry &entry) {
        if (auto checked = fat_files::check_path(entry.path); !checked)
            return std::unexpected(checked.error());
        if (entry.size_bytes > std::numeric_limits<std::uint32_t>::max() || (entry.directory && entry.size_bytes != 0U))
            return std::unexpected(fat_files::error("File size is outside the supported bounds"));
        if (entry.conflict != FileConflict::skip && entry.conflict != FileConflict::replace)
            return std::unexpected(fat_files::error("File conflict policy is invalid"));
        std::string path;
        for (std::size_t i = 0; i + 1U < entry.path.size(); ++i) {
            path += (path.empty() ? "" : "/") + detail::upper_ascii(entry.path[i]);
            const auto parent = nodes_.find(path);
            if (parent == nodes_.end() || !parent->second.directory)
                return std::unexpected(fat_files::error("Destination directory does not exist"));
        }
        path += (path.empty() ? "" : "/") + detail::upper_ascii(entry.path.back());
        const auto found = nodes_.find(path);
        const auto *existing = found == nodes_.end() ? nullptr : &found->second;
        const auto size =
            existing && !existing->directory ? std::optional<std::uint64_t>{existing->size} : std::nullopt;
        if (existing && existing->directory != entry.directory)
            return FilesystemImportDecision{Action::conflict, size,
                                            "A different entry type already occupies this name"};
        if (existing && entry.directory)
            return FilesystemImportDecision{Action::merge_directory, {}, {}};
        const auto issue = existing && existing->incoming ? "An earlier import entry occupies this name" : "";
        if (existing && entry.conflict == FileConflict::skip)
            return FilesystemImportDecision{Action::skip_file, size, issue};
        if (existing && existing->readonly)
            return FilesystemImportDecision{Action::conflict, size, "Read-only files cannot be replaced"};
        if (!existing) {
            if (auto name = fat_files::short_name(entry.path.back()); !name)
                return std::unexpected(name.error());
            if (nodes_.size() >= 100000U)
                return std::unexpected(fat_files::error("FAT entry limit exceeded"));
        }
        const auto action = existing          ? Action::replace_file
                            : entry.directory ? Action::create_directory
                                              : Action::create_file;
        nodes_[path] = {entry.directory, false, true, entry.size_bytes};
        return FilesystemImportDecision{action, size, issue};
    }
    std::map<std::string, ReviewNode> nodes_;
    CancellationToken cancellation_;
};
} // namespace

Result<std::vector<FilesystemImportDecision>> inspect_fat_file_import(std::shared_ptr<const RandomAccessReader> source,
                                                                      PartitionIndex partition,
                                                                      std::span<const FilesystemImportEntry> entries,
                                                                      const CancellationToken &cancellation) {
    if (!source || entries.empty() || entries.size() > 10000U)
        return std::unexpected(fat_files::error("Choose between 1 and 10000 filesystem import entries"));
    auto state = fat_files::open(std::move(source), partition, cancellation);
    if (!state)
        return std::unexpected(state.error());
    return ImportReview{std::move(*state)}.run(entries);
}
} // namespace axk
