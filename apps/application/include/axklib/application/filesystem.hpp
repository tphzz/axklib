#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <vector>

#include "axklib/application/contracts.hpp"
#include "axklib/io.hpp"

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

struct SandboxFile {
    FileRef reference;
    std::string filename;
    std::uint64_t size{};
    std::shared_ptr<const axk::RandomAccessReader> reader;
};

enum class SandboxTreeEntryKind : std::uint8_t { file, directory };

struct SandboxTreeEntry {
    std::string relative_path;
    SandboxTreeEntryKind kind{SandboxTreeEntryKind::file};
    std::uint64_t size{};
};

struct OpenedSandboxTreeFile {
    std::shared_ptr<const axk::RandomAccessReader> reader;
    std::function<Result<void>()> verify_unchanged;
};

struct SandboxTreeLimits {
    std::size_t maximum_entries{};
    std::uint64_t maximum_total_file_bytes{};
    std::size_t maximum_depth{64U};
    std::size_t maximum_path_bytes{32U * 1024U * 1024U};
};

class SandboxTree {
  public:
    SandboxTree();
    ~SandboxTree();
    SandboxTree(SandboxTree &&) noexcept;
    SandboxTree &operator=(SandboxTree &&) noexcept;
    SandboxTree(const SandboxTree &) = delete;
    SandboxTree &operator=(const SandboxTree &) = delete;

    [[nodiscard]] std::span<const SandboxTreeEntry> entries() const noexcept;
    [[nodiscard]] Result<OpenedSandboxTreeFile> open_file(std::size_t index) const;

  private:
    struct Implementation;
    explicit SandboxTree(std::unique_ptr<Implementation> implementation);

    std::unique_ptr<Implementation> implementation_;
    friend class Sandbox;
};

class Sandbox {
  public:
    [[nodiscard]] static Result<Sandbox> create(std::vector<RootDefinition> roots);
    [[nodiscard]] Result<void> replace_roots(std::vector<RootDefinition> roots);

    [[nodiscard]] std::vector<RootInfo> roots() const;
    [[nodiscard]] Result<SandboxFile> open_file(const FileRef &reference) const;
    [[nodiscard]] Result<SandboxTree> open_tree(const DirectoryRef &reference, const SandboxTreeLimits &limits) const;
    [[nodiscard]] Result<void> publish_file(const FileRef &destination, bool overwrite,
                                            const axk::RandomAccessReader &source) const;
    [[nodiscard]] Result<std::filesystem::path> create_staging_directory(std::string_view purpose) const;
    [[nodiscard]] Result<void> publish_directory(const DirectoryRef &destination, bool overwrite,
                                                 const std::filesystem::path &staging) const;
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
    struct NativeRoot;

    struct Root {
        RootInfo info;
        std::filesystem::path canonical_path;
        std::shared_ptr<NativeRoot> native;
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
