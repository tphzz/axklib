#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "axklib/filesystem_edit.hpp"

namespace axk::app {

struct ImageFilesystemAttribute {
    std::string code;
    std::string label;
    std::string value;
    std::string description;
    // Empty summaries keep technical attributes in Storage details only.
    std::string summary;
};

struct ImageFilesystemEntry {
    std::string id;
    std::optional<std::string> parent_id;
    std::string root_id;
    std::vector<std::string> ancestor_ids;
    std::string name;
    std::string path;
    std::string kind;
    std::optional<std::uint64_t> size_bytes;
    std::size_t child_count{};
    std::optional<std::string> object_id;
    std::optional<std::string> content_scope_id;
    std::string interpretation;
    std::string storage;
    std::string issue;
    bool filesystem_metadata{};
    std::string raw_attributes;
    std::vector<ImageFilesystemAttribute> attributes;
};

struct ImageFilesystemQuery {
    std::string parent_id{};
    std::string root_id{};
    std::string query{};
    std::string entry_id{};
    std::string object_id{};
    std::string content_scope_id{};
    std::size_t offset{};
    std::size_t limit{200U};
};

struct ImageFilesystemRootCapabilities {
    std::string root_id{};
    bool create_directory{};
    bool put_file{};
    bool delete_entry{};
    bool rename_entry{};
    std::size_t maximum_name_bytes{};
    std::string name_pattern{};
    std::string name_hint{};
    std::vector<std::string> supported_imports{};
    std::string name_policy{"PRESERVE"};
};

struct ImageFilesystemPage {
    std::uint64_t revision{};
    bool available{};
    std::string filesystem_name;
    std::optional<std::string> device_view;
    std::vector<ImageFilesystemEntry> items;
    std::size_t total_count{};
    std::vector<ImageFilesystemRootCapabilities> root_capabilities;
};

// Destination paths are components relative to an existing directory entry.
// Entry identities are scoped to the image session and expected revision.
struct CreateImageFilesystemDirectory {
    std::string parent_entry_id;
    FilesystemPath relative_path;
};
struct PutImageFilesystemFile {
    std::string parent_entry_id;
    FilesystemPath relative_path;
    std::shared_ptr<const RandomAccessReader> contents;
    FileConflict conflict{FileConflict::skip};
};
struct RemoveImageFilesystemEntry {
    std::string entry_id;
    bool recursive{};
};
struct RenameImageFilesystemEntry {
    std::string entry_id;
    std::string new_name;
};
using ImageFilesystemEdit = std::variant<CreateImageFilesystemDirectory, PutImageFilesystemFile,
                                         RemoveImageFilesystemEntry, RenameImageFilesystemEntry>;

struct ResolvedImageFilesystemEdits {
    PartitionIndex partition;
    std::vector<FilesystemEdit> edits;
};

} // namespace axk::app
