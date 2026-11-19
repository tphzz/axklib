#include "filesystem_internal.hpp"

namespace axk::app {
struct SandboxOutputFile::Binding {
    FileRef destination;
    std::filesystem::path relative;
    bool overwrite{};
    NativeHandle parent;
    NativeIdentity identity;
};

const FileRef &SandboxOutputFile::reference() const noexcept { return binding_->destination; }

Result<SandboxOutputFile> Sandbox::bind_output_file(const FileRef &destination, bool overwrite) const {
    if (auto valid = resolve_output_file(destination, overwrite); !valid)
        return std::unexpected(valid.error());
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
#if defined(_WIN32)
    auto identity = native_identity(parent->get(), destination.relative_path);
#else
    auto identity = native_identity(**parent, destination.relative_path);
#endif
    if (!identity)
        return std::unexpected(identity.error());
    return SandboxOutputFile{std::make_shared<SandboxOutputFile::Binding>(destination, std::move(*relative), overwrite,
                                                                          std::move(*parent), *identity)};
}

Result<PublicationOutcome> Sandbox::publish_file(const FileRef &destination, bool overwrite,
                                                 const axk::RandomAccessReader &source) const {
    auto bound = bind_output_file(destination, overwrite);
    if (!bound)
        return std::unexpected(bound.error());
    return publish_file(*bound, source, {}, {});
}

Result<PublicationOutcome> Sandbox::publish_file(const SandboxOutputFile &output_file,
                                                 const axk::RandomAccessReader &source,
                                                 const CancellationToken &cancellation,
                                                 const std::function<Result<void>()> &before_commit) const {
    const auto &[destination, relative, overwrite, parent, original] = *output_file.binding_;
    const auto active = [&]() -> Result<void> {
        if (auto ok = cancellation.check(); !ok)
            return std::unexpected(Error{"operation_cancelled", ok.error().message});
        return {};
    };
    // Both locks are held only for the binding check and publication. Source
    // verification can itself require a workspace read lease.
    const auto check_bound = [&]() -> Result<void> {
        const auto root = std::ranges::find(state_->roots, destination.root_id,
                                            [](const Root &entry) { return std::string_view{entry.info.id}; });
        if (root != state_->roots.end() && root->info.writable) {
#if defined(_WIN32)
            auto current = open_parent(root->native->handle, relative.parent_path(), destination.relative_path);
            if (current) {
                const auto id = native_identity(current->get(), destination.relative_path);
                if (id && id->volume_serial == original.volume_serial && id->file_id == original.file_id)
                    return {};
            }
#else
            auto current = open_parent(root->native->descriptor, relative.parent_path(), destination.relative_path);
            if (current) {
                const auto id = native_identity(**current, destination.relative_path);
                if (id && id->device == original.device && id->inode == original.inode)
                    return {};
            }
#endif
        }
        return std::unexpected(
            entry_error("output_destination_changed", "Reviewed output directory changed", destination.relative_path));
    };
    if (auto ok = active(); !ok)
        return std::unexpected(ok.error());
    {
        const std::scoped_lock mutation_lock{state_->mutation_mutex};
        const std::shared_lock roots_lock{state_->mutex};
        if (auto bound = check_bound(); !bound)
            return std::unexpected(bound.error());
    }
    for (std::size_t attempt = 0U; attempt < 64U; ++attempt) {
        const auto temporary = temporary_entry_name(relative.filename());
#if defined(_WIN32)
        auto output = open_relative(parent.get(), temporary, FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | DELETE,
                                    FILE_CREATE, FILE_NON_DIRECTORY_FILE, destination.relative_path);
        if (!output)
            continue;
        const auto cleanup = [&] {
            FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
            static_cast<void>(
                SetFileInformationByHandle(output->get(), FileDispositionInfo, &disposition, sizeof(disposition)));
        };
#else
        const auto descriptor =
            ::openat(*parent, temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor < 0) {
            if (errno == EEXIST)
                continue;
            return std::unexpected(
                publication_error("temporary output could not be created", destination.relative_path));
        }
        auto output = descriptor_handle(descriptor);
        const auto cleanup = [&] {
            if (retained_entry_matches(*parent, temporary, descriptor))
                static_cast<void>(::unlinkat(*parent, temporary.c_str(), 0));
        };
#endif
        std::vector<std::byte> buffer(static_cast<std::size_t>(
            std::max<std::uint64_t>(1U, std::min<std::uint64_t>(1024U * 1024U, source.size()))));
        std::uint64_t offset{};
        while (offset < source.size()) {
            if (auto ok = active(); !ok) {
                cleanup();
                return std::unexpected(ok.error());
            }
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), source.size() - offset));
            if (const auto read = source.read_exact_at(offset, std::span{buffer}.first(count)); !read) {
                cleanup();
                return std::unexpected(publication_error(read.error().message, destination.relative_path));
            }
#if defined(_WIN32)
            DWORD written{};
            if (WriteFile(output->get(), buffer.data(), static_cast<DWORD>(count), &written, nullptr) == 0 ||
                written != static_cast<DWORD>(count)) {
                cleanup();
                return std::unexpected(
                    publication_error("temporary output could not be written", destination.relative_path));
            }
#else
            auto remaining = std::span{buffer}.first(count);
            while (!remaining.empty()) {
                const auto written = ::write(descriptor, remaining.data(), remaining.size());
                if (written < 0 && errno == EINTR)
                    continue;
                if (written <= 0) {
                    cleanup();
                    return std::unexpected(
                        publication_error("temporary output could not be written", destination.relative_path));
                }
                remaining = remaining.subspan(static_cast<std::size_t>(written));
            }
#endif
            offset += count;
        }
#if defined(_WIN32)
        const bool flushed = FlushFileBuffers(output->get()) != 0;
#else
        const bool flushed = ::fsync(descriptor) == 0;
#endif
        if (!flushed) {
            cleanup();
            return std::unexpected(
                publication_error("temporary output could not be written and flushed", destination.relative_path));
        }
        if (before_commit) {
            if (auto ok = before_commit(); !ok) {
                cleanup();
                return std::unexpected(ok.error());
            }
        }
        const std::scoped_lock mutation_lock{state_->mutation_mutex};
        const std::shared_lock roots_lock{state_->mutex};
        if (auto bound = check_bound(); !bound) {
            cleanup();
            return std::unexpected(bound.error());
        }
        if (auto ok = active(); !ok) {
            cleanup();
            return std::unexpected(ok.error());
        }
#if defined(_WIN32)
        const auto renamed = rename_open_entry(output->get(), parent.get(), relative.filename(), overwrite);
        if (renamed < 0) {
            const auto error = RtlNtStatusToDosError(renamed);
            cleanup();
            if (!overwrite && (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS))
                return std::unexpected(output_exists_error("output file already exists", destination.relative_path));
            return std::unexpected(
                publication_error("output could not be published atomically", destination.relative_path));
        }
#else
        if (!retained_entry_matches(*parent, temporary, descriptor))
            return std::unexpected(
                publication_error("temporary output changed before publication", destination.relative_path));
        const auto published =
            overwrite ? ::renameat(*parent, temporary.c_str(), *parent, relative.filename().c_str())
                      : rename_no_replace(*parent, temporary.c_str(), *parent, relative.filename().c_str());
        if (published != 0) {
            const auto error = errno;
            cleanup();
            if (!overwrite && error == EEXIST)
                return std::unexpected(output_exists_error("output file already exists", destination.relative_path));
            return std::unexpected(
                publication_error("output could not be published atomically", destination.relative_path));
        }
        if (::fsync(*parent) != 0)
            return PublicationOutcome{
                PublicationDurability::unconfirmed,
                {{"publication_durability_unconfirmed", "File published; output directory synchronization failed"}}};
#endif
        return {};
    }
    return std::unexpected(
        publication_error("a unique temporary output could not be reserved", destination.relative_path));
}
} // namespace axk::app

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
