#include "filesystem_internal.hpp"

axk::app::Result<void> axk::app::Sandbox::publish_file(const FileRef &destination, bool overwrite,
                                                       const axk::RandomAccessReader &source) const {
    const std::scoped_lock mutation_lock{state_->mutation_mutex};
    const auto root = find_root(destination.root_id);
    if (!root)
        return std::unexpected(reference_error("sandbox root does not exist", destination.relative_path));
    if (!root->info.writable)
        return std::unexpected(entry_error("read_only_root", "sandbox root is read-only", destination.relative_path));
    auto relative = relative_path_from_utf8(destination.relative_path);
    if (!relative || relative->filename().empty())
        return std::unexpected(relative ? reference_error("output file requires a filename", destination.relative_path)
                                        : relative.error());
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
        auto output = open_relative(parent->get(), temporary, FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | DELETE,
                                    FILE_CREATE, FILE_NON_DIRECTORY_FILE, destination.relative_path);
        if (!output)
            continue;
        const auto cleanup = [&] {
            FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
            static_cast<void>(
                SetFileInformationByHandle(output->get(), FileDispositionInfo, &disposition, sizeof(disposition)));
        };
        std::vector<std::byte> buffer(static_cast<std::size_t>(
            std::max<std::uint64_t>(1U, std::min<std::uint64_t>(1024U * 1024U, source.size()))));
        std::uint64_t offset{};
        while (offset < source.size()) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), source.size() - offset));
            if (const auto read = source.read_exact_at(offset, std::span{buffer}.first(count)); !read) {
                cleanup();
                return std::unexpected(publication_error(read.error().message, destination.relative_path));
            }
            DWORD written{};
            if (WriteFile(output->get(), buffer.data(), static_cast<DWORD>(count), &written, nullptr) == 0 ||
                written != static_cast<DWORD>(count)) {
                cleanup();
                return std::unexpected(
                    publication_error("temporary output could not be written", destination.relative_path));
            }
            offset += count;
        }
        if (FlushFileBuffers(output->get()) == 0) {
            cleanup();
            return std::unexpected(
                publication_error("temporary output could not be flushed", destination.relative_path));
        }
        const auto renamed = rename_open_entry(output->get(), parent->get(), relative->filename(), overwrite);
        if (renamed < 0) {
            const auto error = RtlNtStatusToDosError(renamed);
            cleanup();
            if (!overwrite && (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS))
                return std::unexpected(output_exists_error("output file already exists", destination.relative_path));
            return std::unexpected(
                publication_error("output could not be published atomically", destination.relative_path));
        }
#else
        const auto descriptor =
            ::openat(**parent, temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor < 0) {
            if (errno == EEXIST)
                continue;
            return std::unexpected(
                publication_error("temporary output could not be created", destination.relative_path));
        }
        const auto cleanup = [&] { static_cast<void>(::unlinkat(**parent, temporary.c_str(), 0)); };
        std::vector<std::byte> buffer(static_cast<std::size_t>(
            std::max<std::uint64_t>(1U, std::min<std::uint64_t>(1024U * 1024U, source.size()))));
        std::uint64_t offset{};
        bool failed{};
        while (offset < source.size() && !failed) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), source.size() - offset));
            if (const auto read = source.read_exact_at(offset, std::span{buffer}.first(count)); !read) {
                ::close(descriptor);
                cleanup();
                return std::unexpected(publication_error(read.error().message, destination.relative_path));
            }
            auto remaining = std::span{buffer}.first(count);
            while (!remaining.empty()) {
                const auto written = ::write(descriptor, remaining.data(), remaining.size());
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
        if (failed || ::fsync(descriptor) != 0) {
            ::close(descriptor);
            cleanup();
            return std::unexpected(
                publication_error("temporary output could not be written and flushed", destination.relative_path));
        }
        ::close(descriptor);
        const auto published =
            overwrite ? ::renameat(**parent, temporary.c_str(), **parent, relative->filename().c_str())
                      : rename_no_replace(**parent, temporary.c_str(), **parent, relative->filename().c_str());
        if (published != 0) {
            const auto error = errno;
            cleanup();
            if (!overwrite && error == EEXIST)
                return std::unexpected(output_exists_error("output file already exists", destination.relative_path));
            return std::unexpected(
                publication_error("output could not be published atomically", destination.relative_path));
        }
        if (::fsync(**parent) != 0)
            return std::unexpected(
                publication_error("published output directory could not be synchronized", destination.relative_path));
#endif
        return {};
    }
    return std::unexpected(
        publication_error("a unique temporary output could not be reserved", destination.relative_path));
}

axk::app::Result<std::filesystem::path> axk::app::Sandbox::create_staging_directory(std::string_view purpose) const {
    auto purpose_path = entry_name_from_utf8(purpose);
    if (!purpose_path)
        return std::unexpected(purpose_path.error());
    std::error_code error;
    const auto temporary_root = std::filesystem::temp_directory_path(error);
    if (error)
        return std::unexpected(publication_error("temporary directory is unavailable", purpose));
    for (std::size_t attempt = 0U; attempt < 64U; ++attempt) {
        const auto candidate = temporary_root / temporary_entry_name(*purpose_path);
        if (!std::filesystem::create_directory(candidate, error)) {
            if (!error)
                continue;
            return std::unexpected(publication_error("private staging directory could not be created", purpose));
        }
#if !defined(_WIN32)
        std::filesystem::permissions(candidate, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, error);
        if (error) {
            std::filesystem::remove(candidate, error);
            return std::unexpected(publication_error("private staging directory could not be secured", purpose));
        }
#endif
        return candidate;
    }
    return std::unexpected(publication_error("a unique private staging directory could not be reserved", purpose));
}
