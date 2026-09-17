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

#include "sfs_files_internal.hpp"

namespace axk {
namespace {
using Action = FilesystemImportAction;
using Decision = FilesystemImportDecision;
using Key = std::pair<std::uint64_t, std::string>;

struct Node {
    bool directory{};
    std::optional<std::uint32_t> record;
    std::uint64_t size{};
};

class ImportReview {
  public:
    explicit ImportReview(sfs_files::State state) : state_(std::move(state)) {
        nodes_.emplace(state_.root.value, Node{true, state_.root.value, 0U});
    }

    Result<std::vector<Decision>> run(std::span<const FilesystemImportEntry> entries) {
        std::vector<Decision> result;
        result.reserve(entries.size());
        for (const auto &entry : entries) {
            if (auto checked = state_.cancellation.check(); !checked)
                return std::unexpected(checked.error());
            auto decision = inspect(entry);
            if (!decision && decision.error().code == ErrorCode::operation_cancelled)
                return std::unexpected(decision.error());
            if (exceeded_)
                return std::unexpected(sfs_files::error("import review exceeds its metadata limit"));
            result.push_back(decision ? std::move(*decision)
                                      : Decision{Action::conflict, {}, decision.error().message});
        }
        return result;
    }

  private:
    bool protected_node(std::uint64_t id) const {
        const auto &node = nodes_.at(id);
        return node.record && state_.protected_records.contains(*node.record);
    }

    Result<std::optional<std::uint64_t>> child(const Key &key) {
        if (const auto found = children_.find(key); found != children_.end())
            return found->second;
        // Bound cache overhead as well as the request count/path lengths.
        if (++cache_entries_ > 250000U) {
            exceeded_ = true;
            return std::unexpected(sfs_files::error("import review exceeds its metadata limit"));
        }
        std::optional<std::uint64_t> target;
        const auto &parent = nodes_.at(key.first);
        if (parent.record) {
            auto [directory, inserted] = directories_.try_emplace(*parent.record);
            if (inserted) {
                for (const auto &entry : state_.records.at(*parent.record).info.directory_entries) {
                    if (auto checked = state_.cancellation.check(); !checked)
                        return std::unexpected(checked.error());
                    if (entry.state != DirectoryEntryState::live || entry.name == "." || entry.name == "..")
                        continue;
                    if (++cache_entries_ > 250000U) {
                        exceeded_ = true;
                        return std::unexpected(sfs_files::error("import review exceeds its metadata limit"));
                    }
                    auto [found, unique] = directory->second.emplace(entry.name, entry.raw_link_id.value);
                    if (!unique)
                        found->second.reset();
                }
            }
            const auto found = directory->second.find(key.second);
            if (found != directory->second.end()) {
                if (!found->second)
                    return std::unexpected(sfs_files::error("filesystem path has duplicate names"));
                const auto record = state_.records.find(*found->second);
                if (record == state_.records.end())
                    return std::unexpected(sfs_files::error("filesystem entry target is missing"));
                target = record->first;
                const auto &info = record->second.info;
                nodes_.emplace(*target,
                               Node{info.payload_kind == PayloadKind::directory, record->first, info.data_size});
            }
        }
        children_.emplace(key, target);
        return target;
    }

    Result<Decision> inspect(const FilesystemImportEntry &entry) {
        if (auto checked = sfs_files::check_path(entry.path); !checked)
            return std::unexpected(checked.error());
        if (entry.size_bytes > std::numeric_limits<std::uint32_t>::max() || (entry.directory && entry.size_bytes != 0U))
            return std::unexpected(sfs_files::error("file size is outside the supported bounds"));
        if (entry.conflict != FileConflict::skip && entry.conflict != FileConflict::replace)
            return std::unexpected(sfs_files::error("file conflict policy is invalid"));
        std::uint64_t parent = state_.root.value;
        for (std::size_t i = 0; i + 1U < entry.path.size(); ++i) {
            auto next = child({parent, entry.path[i]});
            if (!next)
                return std::unexpected(next.error());
            if (!*next)
                return std::unexpected(sfs_files::error("filesystem destination directory does not exist"));
            parent = **next;
            if (!nodes_.at(parent).directory)
                return std::unexpected(sfs_files::error("filesystem path contains a file instead of a directory"));
            if (protected_node(parent))
                return std::unexpected(sfs_files::error("filesystem metadata is protected from raw edits"));
        }
        const Key key{parent, entry.path.back()};
        const auto target = child(key);
        if (!target)
            return std::unexpected(target.error());
        const Node *existing = *target ? &nodes_.at(**target) : nullptr;
        if (*target && protected_node(**target))
            return std::unexpected(sfs_files::error("filesystem metadata is protected from raw edits"));
        const auto size =
            existing && !existing->directory ? std::optional<std::uint64_t>{existing->size} : std::nullopt;
        if (existing && existing->directory != entry.directory)
            return Decision{Action::conflict, size,
                            entry.directory ? "A file already occupies the directory name"
                                            : "A directory already occupies the file name"};
        const auto issue =
            existing && !existing->record && !entry.directory ? "An earlier import entry occupies this name" : "";
        if (existing && entry.directory)
            return Decision{Action::merge_directory, {}, {}};
        if (existing && entry.conflict == FileConflict::skip)
            return Decision{Action::skip_file, size, issue};
        const auto id = next_virtual_++;
        nodes_.emplace(id, Node{entry.directory, {}, entry.size_bytes});
        children_[key] = id;
        return Decision{existing          ? Action::replace_file
                        : entry.directory ? Action::create_directory
                                          : Action::create_file,
                        size, issue};
    }

    sfs_files::State state_;
    std::map<std::uint64_t, Node> nodes_;
    std::map<Key, std::optional<std::uint64_t>> children_;
    std::map<std::uint32_t, std::map<std::string, std::optional<std::uint32_t>>> directories_;
    std::uint64_t next_virtual_{static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U};
    bool exceeded_{};
    std::size_t cache_entries_{};
};
} // namespace

Result<std::vector<FilesystemImportDecision>> inspect_sfs_file_import(std::shared_ptr<const RandomAccessReader> source,
                                                                      PartitionIndex partition,
                                                                      std::span<const FilesystemImportEntry> entries,
                                                                      const CancellationToken &cancellation) {
    if (!source || entries.empty() || entries.size() > 10000U)
        return std::unexpected(sfs_files::error("choose between 1 and 10000 filesystem import entries"));
    auto state = sfs_files::open(std::move(source), partition, cancellation);
    if (!state)
        return std::unexpected(state.error());
    return ImportReview{std::move(*state)}.run(entries);
}
} // namespace axk
