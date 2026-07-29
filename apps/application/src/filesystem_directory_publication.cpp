#include "filesystem_internal.hpp"

axk::app::Result<void> axk::app::Sandbox::publish_directory(const DirectoryRef &destination, bool overwrite,
                                                            const std::filesystem::path &staging) const {
    struct StagedEntry {
        std::filesystem::path source;
        std::filesystem::path relative;
        bool directory{};
    };

    std::error_code error;
    const auto staging_status = std::filesystem::symlink_status(staging, error);
    if (error || !std::filesystem::is_directory(staging_status) || std::filesystem::is_symlink(staging_status)) {
        return std::unexpected(publication_error("staging directory is unavailable", destination.relative_path));
    }
    std::vector<StagedEntry> entries;
    for (std::filesystem::recursive_directory_iterator iterator{staging, error}, end; iterator != end && !error;
         iterator.increment(error)) {
        const auto status = iterator->symlink_status(error);
        if (error || std::filesystem::is_symlink(status) ||
            (!std::filesystem::is_directory(status) && !std::filesystem::is_regular_file(status))) {
            return std::unexpected(
                publication_error("staging directory contains an unsupported entry", destination.relative_path));
        }
        const auto relative = iterator->path().lexically_relative(staging);
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
            return std::unexpected(
                publication_error("staging directory contains an invalid path", destination.relative_path));
        }
        entries.push_back({iterator->path(), relative, std::filesystem::is_directory(status)});
    }
    if (error)
        return std::unexpected(
            publication_error("staging directory could not be enumerated", destination.relative_path));
    std::ranges::sort(entries, [](const auto &left, const auto &right) {
        const auto left_depth = static_cast<std::size_t>(std::distance(left.relative.begin(), left.relative.end()));
        const auto right_depth = static_cast<std::size_t>(std::distance(right.relative.begin(), right.relative.end()));
        return std::tie(left_depth, left.relative) < std::tie(right_depth, right.relative);
    });

    const std::scoped_lock mutation_lock{state_->mutation_mutex};
    const auto root = find_root(destination.root_id);
    if (!root)
        return std::unexpected(reference_error("sandbox root does not exist", destination.relative_path));
    if (!root->info.writable)
        return std::unexpected(entry_error("read_only_root", "sandbox root is read-only", destination.relative_path));
    auto relative = relative_path_from_utf8(destination.relative_path);
    if (!relative || relative->filename().empty()) {
        return std::unexpected(relative ? reference_error("output directory requires a name", destination.relative_path)
                                        : relative.error());
    }
#if defined(_WIN32)
    auto parent = open_parent(root->native->handle, relative->parent_path(), destination.relative_path);
#else
    auto parent = open_parent(root->native->descriptor, relative->parent_path(), destination.relative_path);
#endif
    if (!parent)
        return std::unexpected(parent.error());

    for (std::size_t attempt = 0U; attempt < 64U; ++attempt) {
        const auto temporary = temporary_entry_name(relative->filename());
#if defined(_WIN32)
        auto staged = open_relative(parent->get(), temporary,
                                    FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | DELETE,
                                    FILE_CREATE, FILE_DIRECTORY_FILE, destination.relative_path);
        if (!staged)
            continue;
        const auto discard_staged = [&] {
            static_cast<void>(delete_open_tree(staged->get(), destination.relative_path));
            FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
            static_cast<void>(
                SetFileInformationByHandle(staged->get(), FileDispositionInfo, &disposition, sizeof(disposition)));
        };
        for (const auto &entry : entries) {
            auto entry_parent = open_parent(staged->get(), entry.relative.parent_path(), destination.relative_path);
            if (!entry_parent) {
                discard_staged();
                return std::unexpected(entry_parent.error());
            }
            if (entry.directory) {
                auto created =
                    open_relative(entry_parent->get(), entry.relative.filename(),
                                  FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | DELETE,
                                  FILE_CREATE, FILE_DIRECTORY_FILE, destination.relative_path);
                if (!created) {
                    discard_staged();
                    return std::unexpected(
                        publication_error("staged directory could not be created", destination.relative_path));
                }
                continue;
            }
            auto input = axk::FileReader::open(entry.source);
            auto output = open_relative(entry_parent->get(), entry.relative.filename(),
                                        FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | DELETE, FILE_CREATE,
                                        FILE_NON_DIRECTORY_FILE, destination.relative_path);
            if (!input || !output) {
                discard_staged();
                return std::unexpected(
                    publication_error("staged output file could not be opened", destination.relative_path));
            }
            std::vector<std::byte> buffer(static_cast<std::size_t>(
                std::max<std::uint64_t>(1U, std::min<std::uint64_t>(1024U * 1024U, (*input)->size()))));
            std::uint64_t offset{};
            while (offset < (*input)->size()) {
                const auto count =
                    static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), (*input)->size() - offset));
                if (const auto read = (*input)->read_exact_at(offset, std::span{buffer}.first(count)); !read) {
                    discard_staged();
                    return std::unexpected(publication_error(read.error().message, destination.relative_path));
                }
                DWORD written{};
                if (WriteFile(output->get(), buffer.data(), static_cast<DWORD>(count), &written, nullptr) == 0 ||
                    written != static_cast<DWORD>(count)) {
                    discard_staged();
                    return std::unexpected(
                        publication_error("staged output file could not be written", destination.relative_path));
                }
                offset += count;
            }
            if (FlushFileBuffers(output->get()) == 0) {
                discard_staged();
                return std::unexpected(
                    publication_error("staged output file could not be flushed", destination.relative_path));
            }
        }

        auto existing =
            open_relative(parent->get(), relative->filename(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | DELETE,
                          FILE_OPEN, FILE_DIRECTORY_FILE, destination.relative_path);
        if (existing && !overwrite) {
            const auto empty = directory_is_empty(existing->get());
            if (!empty || !*empty) {
                discard_staged();
                return std::unexpected(
                    output_exists_error("output directory already exists", destination.relative_path));
            }
            existing =
                open_relative(parent->get(), relative->filename(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | DELETE,
                              FILE_OPEN, FILE_DIRECTORY_FILE, destination.relative_path);
            if (!existing) {
                discard_staged();
                return std::unexpected(
                    publication_error("output directory changed during publication", destination.relative_path));
            }
        }
        if (!existing) {
            if (rename_open_entry(staged->get(), parent->get(), relative->filename(), false) < 0) {
                discard_staged();
                return std::unexpected(
                    publication_error("output directory could not be published atomically", destination.relative_path));
            }
            return {};
        }

        const auto backup = temporary_entry_name(relative->filename());
        if (rename_open_entry(existing->get(), parent->get(), backup, false) < 0) {
            discard_staged();
            return std::unexpected(
                publication_error("existing output directory could not be reserved", destination.relative_path));
        }
        if (!overwrite) {
            auto inspection = open_relative(parent->get(), backup, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                            FILE_OPEN, FILE_DIRECTORY_FILE, destination.relative_path);
            const auto empty = inspection ? directory_is_empty(inspection->get()) : std::nullopt;
            if (!empty || !*empty) {
                static_cast<void>(rename_open_entry(existing->get(), parent->get(), relative->filename(), false));
                discard_staged();
                return std::unexpected(
                    output_exists_error("output directory changed during publication", destination.relative_path));
            }
        }
        if (rename_open_entry(staged->get(), parent->get(), relative->filename(), false) < 0) {
            static_cast<void>(rename_open_entry(existing->get(), parent->get(), relative->filename(), false));
            discard_staged();
            return std::unexpected(
                publication_error("output directory could not be published atomically", destination.relative_path));
        }
        if (!delete_open_tree(existing->get(), destination.relative_path)) {
            return std::unexpected(
                publication_error("replaced output directory could not be removed", destination.relative_path));
        }
        FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
        if (SetFileInformationByHandle(existing->get(), FileDispositionInfo, &disposition, sizeof(disposition)) == 0) {
            return std::unexpected(
                publication_error("replaced output directory could not be removed", destination.relative_path));
        }
#else
        if (::mkdirat(**parent, temporary.c_str(), 0700) != 0) {
            if (errno == EEXIST)
                continue;
            return std::unexpected(
                publication_error("temporary output directory could not be created", destination.relative_path));
        }
        const auto staged_descriptor =
            ::openat(**parent, temporary.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (staged_descriptor < 0) {
            static_cast<void>(::unlinkat(**parent, temporary.c_str(), AT_REMOVEDIR));
            return std::unexpected(
                publication_error("temporary output directory could not be opened", destination.relative_path));
        }
        auto staged = descriptor_handle(staged_descriptor);
        struct stat staged_status{};
        if (::fstat(*staged, &staged_status) != 0 || !S_ISDIR(staged_status.st_mode)) {
            staged.reset();
            static_cast<void>(::unlinkat(**parent, temporary.c_str(), AT_REMOVEDIR));
            return std::unexpected(publication_error("temporary output directory identity could not be retained",
                                                     destination.relative_path));
        }
        const auto staged_identity = object_identity(staged_status);
        const auto discard_staged = [&] { static_cast<void>(delete_tree_at(**parent, temporary, staged_identity)); };
        for (const auto &entry : entries) {
            auto entry_parent = open_parent(*staged, entry.relative.parent_path(), destination.relative_path);
            if (!entry_parent) {
                staged.reset();
                discard_staged();
                return std::unexpected(entry_parent.error());
            }
            if (entry.directory) {
                if (::mkdirat(**entry_parent, entry.relative.filename().c_str(), 0700) != 0) {
                    staged.reset();
                    discard_staged();
                    return std::unexpected(
                        publication_error("staged directory could not be created", destination.relative_path));
                }
                continue;
            }
            auto input = axk::FileReader::open(entry.source);
            const auto output = ::openat(**entry_parent, entry.relative.filename().c_str(),
                                         O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (!input || output < 0) {
                if (output >= 0)
                    ::close(output);
                staged.reset();
                discard_staged();
                return std::unexpected(
                    publication_error("staged output file could not be opened", destination.relative_path));
            }
            std::vector<std::byte> buffer(static_cast<std::size_t>(
                std::max<std::uint64_t>(1U, std::min<std::uint64_t>(1024U * 1024U, (*input)->size()))));
            std::uint64_t offset{};
            bool failed{};
            while (offset < (*input)->size() && !failed) {
                const auto count =
                    static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), (*input)->size() - offset));
                if (const auto read = (*input)->read_exact_at(offset, std::span{buffer}.first(count)); !read) {
                    failed = true;
                    break;
                }
                auto remaining = std::span{buffer}.first(count);
                while (!remaining.empty()) {
                    const auto written = ::write(output, remaining.data(), remaining.size());
                    if (written < 0 && errno == EINTR)
                        continue;
                    if (written <= 0) {
                        failed = true;
                        break;
                    }
                    remaining = remaining.subspan(static_cast<std::size_t>(written));
                }
                offset += count;
            }
            if (failed || ::fsync(output) != 0) {
                ::close(output);
                staged.reset();
                discard_staged();
                return std::unexpected(
                    publication_error("staged output file could not be written", destination.relative_path));
            }
            ::close(output);
        }
        if (!synchronize_directory_tree(*staged)) {
            staged.reset();
            discard_staged();
            return std::unexpected(
                publication_error("temporary output directory could not be synchronized", destination.relative_path));
        }
        staged.reset();

        struct stat destination_status{};
        const auto destination_exists =
            ::fstatat(**parent, relative->filename().c_str(), &destination_status, AT_SYMLINK_NOFOLLOW) == 0;
        if (!destination_exists && errno != ENOENT) {
            discard_staged();
            return std::unexpected(
                publication_error("output directory could not be inspected", destination.relative_path));
        }
        if (destination_exists && (!S_ISDIR(destination_status.st_mode) || S_ISLNK(destination_status.st_mode))) {
            discard_staged();
            return std::unexpected(
                reference_error("output directory is not a regular directory", destination.relative_path));
        }
        if (destination_exists && !overwrite) {
            const auto empty = directory_empty_at(**parent, relative->filename(), object_identity(destination_status));
            if (!empty || !*empty) {
                discard_staged();
                return std::unexpected(
                    output_exists_error("output directory already exists", destination.relative_path));
            }
        }
        const auto published =
            destination_exists ? rename_exchange(**parent, temporary.c_str(), relative->filename().c_str())
                               : rename_no_replace(**parent, temporary.c_str(), **parent, relative->filename().c_str());
        if (published != 0) {
            const auto publish_error = errno;
            discard_staged();
            if (!overwrite && publish_error == EEXIST)
                return std::unexpected(
                    output_exists_error("output directory already exists", destination.relative_path));
            return std::unexpected(
                publication_error("output directory could not be published atomically", destination.relative_path));
        }
        if (destination_exists && !overwrite) {
            const auto destination_identity = object_identity(destination_status);
            const auto displaced_empty = directory_empty_at(**parent, temporary, destination_identity);
            if (!displaced_empty || !*displaced_empty) {
                const auto current_temporary = object_identity_at(**parent, temporary);
                const auto current_destination = object_identity_at(**parent, relative->filename());
                if (current_temporary && *current_temporary == destination_identity && current_destination &&
                    *current_destination == staged_identity &&
                    rename_exchange(**parent, temporary.c_str(), relative->filename().c_str()) == 0) {
                    discard_staged();
                }
                return std::unexpected(
                    output_exists_error("output directory changed during publication", destination.relative_path));
            }
        }
        if (destination_exists && !delete_tree_at(**parent, temporary, object_identity(destination_status))) {
            return std::unexpected(
                publication_error("replaced output directory could not be removed", destination.relative_path));
        }
        if (::fsync(**parent) != 0) {
            return std::unexpected(
                publication_error("published output directory could not be synchronized", destination.relative_path));
        }
#endif
        return {};
    }
    return std::unexpected(
        publication_error("a unique temporary output directory could not be reserved", destination.relative_path));
}
