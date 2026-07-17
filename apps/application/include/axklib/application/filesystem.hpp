#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "axklib/application/contracts.hpp"

namespace axk::app {

struct RootDefinition {
    std::string id;
    std::string display_name;
    std::filesystem::path path;
    bool writable{true};
};

struct RootInfo {
    std::string id;
    std::string display_name;
    bool writable{};
};

enum class DirectoryEntryKind : std::uint8_t { file, directory };

struct DirectoryEntry {
    std::string name;
    std::string relative_path;
    DirectoryEntryKind kind{DirectoryEntryKind::file};
    std::optional<std::uint64_t> size;
};

struct DirectoryListing {
    DirectoryRef directory;
    std::vector<DirectoryEntry> entries;
    bool truncated{};
    std::optional<std::string> next_cursor;
};

struct EntryMetadata {
    std::string root_id;
    std::string relative_path;
    DirectoryEntryKind kind{DirectoryEntryKind::file};
    std::optional<std::uint64_t> size;
    bool writable{};
};

class Sandbox {
  public:
    [[nodiscard]] static Result<Sandbox> create(std::vector<RootDefinition> roots);
    [[nodiscard]] Result<void> replace_roots(std::vector<RootDefinition> roots);

    [[nodiscard]] std::vector<RootInfo> roots() const;
    [[nodiscard]] Result<std::filesystem::path> resolve_file(const FileRef &reference) const;
    [[nodiscard]] Result<std::filesystem::path> resolve_directory(const DirectoryRef &reference) const;
    [[nodiscard]] Result<std::filesystem::path> resolve_output_file(const FileRef &reference, bool overwrite) const;
    [[nodiscard]] Result<std::filesystem::path> resolve_output_directory(const DirectoryRef &reference,
                                                                         bool overwrite) const;
    [[nodiscard]] Result<EntryMetadata> metadata(std::string_view root_id, std::string_view relative_path) const;
    [[nodiscard]] Result<DirectoryListing> list_directory(const DirectoryRef &reference, std::size_t limit,
                                                          std::optional<std::string_view> cursor = std::nullopt) const;
    [[nodiscard]] Result<EntryMetadata> create_directory(const DirectoryRef &parent, std::string_view name) const;
    [[nodiscard]] Result<EntryMetadata> rename_entry(const FileRef &reference, std::string_view name) const;
    [[nodiscard]] Result<void> delete_entry(const FileRef &reference) const;
    [[nodiscard]] Result<void> require_distinct(const FileRef &source, const FileRef &destination) const;
    [[nodiscard]] Result<std::size_t> cleanup_abandoned_publications() const;

  private:
    struct Root {
        RootInfo info;
        std::filesystem::path canonical_path;
    };

    struct State {
        mutable std::shared_mutex mutex;
        mutable std::mutex mutation_mutex;
        std::vector<Root> roots;
    };

    explicit Sandbox(std::vector<Root> roots) : state_(std::make_shared<State>()) { state_->roots = std::move(roots); }

    [[nodiscard]] static Result<std::vector<Root>> validate_roots(std::vector<RootDefinition> roots);
    [[nodiscard]] std::optional<Root> find_root(std::string_view root_id) const;
    [[nodiscard]] Result<std::filesystem::path> resolve_existing(std::string_view root_id,
                                                                 std::string_view relative_path) const;

    std::shared_ptr<State> state_;
};

[[nodiscard]] std::string_view directory_entry_kind_name(DirectoryEntryKind kind) noexcept;

} // namespace axk::app
