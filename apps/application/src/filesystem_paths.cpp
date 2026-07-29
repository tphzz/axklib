#include "filesystem_internal.hpp"

axk::app::Result<std::filesystem::path> axk::app::Sandbox::resolve_existing(std::string_view root_id,
                                                                            std::string_view relative_path) const {
    const auto root = find_root(root_id);
    if (!root)
        return std::unexpected(reference_error("sandbox root does not exist", relative_path));
    auto relative = relative_path_from_utf8(relative_path);
    if (!relative)
        return std::unexpected(relative.error());
    if (auto verified = verify_no_link_components(root->canonical_path, *relative, relative_path); !verified)
        return std::unexpected(verified.error());

    std::error_code error;
    const auto candidate = std::filesystem::weakly_canonical(root->canonical_path / *relative, error);
    if (error || !std::filesystem::exists(candidate, error) || error)
        return std::unexpected(entry_error("entry_not_found", "sandbox entry does not exist", relative_path));
    if (!within(root->canonical_path, candidate))
        return std::unexpected(reference_error("sandbox entry escapes its configured root", relative_path));
    return candidate;
}

axk::app::Result<std::filesystem::path> axk::app::Sandbox::resolve_file(const FileRef &reference) const {
    if (reference.relative_path.empty())
        return std::unexpected(reference_error("file reference requires a relative path", reference.relative_path));
    auto result = resolve_existing(reference.root_id, reference.relative_path);
    if (!result)
        return result;
    std::error_code error;
    if (!std::filesystem::is_regular_file(*result, error) || error)
        return std::unexpected(reference_error("file reference does not name a regular file", reference.relative_path));
    return result;
}

axk::app::Result<std::filesystem::path> axk::app::Sandbox::resolve_directory(const DirectoryRef &reference) const {
    auto result = resolve_existing(reference.root_id, reference.relative_path);
    if (!result)
        return result;
    std::error_code error;
    if (!std::filesystem::is_directory(*result, error) || error)
        return std::unexpected(
            reference_error("directory reference does not name a directory", reference.relative_path));
    return result;
}

axk::app::Result<std::filesystem::path> axk::app::Sandbox::resolve_output_file(const FileRef &reference,
                                                                               bool overwrite) const {
    const auto root = find_root(reference.root_id);
    if (!root)
        return std::unexpected(reference_error("sandbox root does not exist", reference.relative_path));
    if (!root->info.writable)
        return std::unexpected(reference_error("sandbox root is read-only", reference.relative_path));
    if (reference.relative_path.empty())
        return std::unexpected(reference_error("output file requires a relative path", reference.relative_path));
    auto relative = relative_path_from_utf8(reference.relative_path);
    if (!relative)
        return std::unexpected(relative.error());
    if (relative->filename().empty())
        return std::unexpected(reference_error("output file requires a filename", reference.relative_path));

    if (auto verified =
            verify_no_link_components(root->canonical_path, relative->parent_path(), reference.relative_path);
        !verified) {
        return std::unexpected(verified.error());
    }

    std::error_code error;
    const auto parent = std::filesystem::canonical(root->canonical_path / relative->parent_path(), error);
    if (error || !std::filesystem::is_directory(parent, error) || error || !within(root->canonical_path, parent))
        return std::unexpected(reference_error("output parent is not a sandbox directory", reference.relative_path));
    const auto candidate = parent / relative->filename();
    const auto status = std::filesystem::symlink_status(candidate, error);
    if (error && error != std::errc::no_such_file_or_directory)
        return std::unexpected(reference_error("output path cannot be inspected", reference.relative_path));
    if (!error && std::filesystem::exists(status)) {
        if (!overwrite)
            return std::unexpected(output_exists_error("output file already exists", reference.relative_path));
        if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
            return std::unexpected(reference_error("output path is not a regular file", reference.relative_path));
    }
    return candidate;
}

axk::app::Result<std::filesystem::path> axk::app::Sandbox::resolve_output_directory(const DirectoryRef &reference,
                                                                                    bool overwrite) const {
    const auto root = find_root(reference.root_id);
    if (!root)
        return std::unexpected(reference_error("sandbox root does not exist", reference.relative_path));
    if (!root->info.writable)
        return std::unexpected(reference_error("sandbox root is read-only", reference.relative_path));
    if (reference.relative_path.empty())
        return std::unexpected(reference_error("output directory requires a relative path", reference.relative_path));
    auto relative = relative_path_from_utf8(reference.relative_path);
    if (!relative)
        return std::unexpected(relative.error());
    if (relative->filename().empty()) {
        return std::unexpected(reference_error("output directory requires a name", reference.relative_path));
    }
    if (auto verified =
            verify_no_link_components(root->canonical_path, relative->parent_path(), reference.relative_path);
        !verified) {
        return std::unexpected(verified.error());
    }
    std::error_code error;
    const auto parent = std::filesystem::canonical(root->canonical_path / relative->parent_path(), error);
    if (error || !std::filesystem::is_directory(parent, error) || error || !within(root->canonical_path, parent)) {
        return std::unexpected(reference_error("output parent is not a sandbox directory", reference.relative_path));
    }
    const auto candidate = parent / relative->filename();
    const auto status = std::filesystem::symlink_status(candidate, error);
    if (error && error != std::errc::no_such_file_or_directory)
        return std::unexpected(reference_error("output path cannot be inspected", reference.relative_path));
    if (!error && std::filesystem::exists(status)) {
        if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
            return std::unexpected(reference_error("output path is not a directory", reference.relative_path));
        }
        if (!overwrite) {
            const auto empty = std::filesystem::is_empty(candidate, error);
            if (error || !empty)
                return std::unexpected(
                    output_exists_error("output directory already exists and is not empty", reference.relative_path));
        }
    }
    return candidate;
}
