#include "filesystem_internal.hpp"

axk::app::SandboxMutation::SandboxMutation(std::unique_ptr<Implementation> implementation)
    : implementation_(std::move(implementation)) {}

axk::app::SandboxMutation::~SandboxMutation() = default;

std::uint64_t axk::app::SandboxMutation::size() const noexcept {
    return implementation_ == nullptr ? 0U : implementation_->size;
}

const axk::app::FileRef &axk::app::SandboxMutation::reference() const noexcept { return implementation_->reference; }

std::string axk::app::SandboxMutation::stable_identity() const {
    if (!implementation_)
        return {};
    std::ostringstream output;
    output << std::hex << std::setfill('0');
#if defined(_WIN32)
    output << std::setw(16) << implementation_->identity.volume_serial << ':';
    for (const auto value : implementation_->identity.file_id)
        output << std::setw(2) << std::to_integer<unsigned int>(value);
#else
    output << std::setw(16) << implementation_->identity.device << ':' << std::setw(16)
           << implementation_->identity.inode;
#endif
    return output.str();
}

axk::Result<void> axk::app::SandboxMutation::read_exact_at(std::uint64_t offset,
                                                           std::span<std::byte> destination) const {
    if (!implementation_ || offset > size() || destination.size() > size() - offset) {
        return std::unexpected{axk::make_error(axk::ErrorCode::io_short_read, axk::ErrorCategory::io,
                                               "sandbox mutation read exceeds available data")};
    }
    const std::scoped_lock lock{implementation_->io_mutex};
    auto remaining = destination;
#if defined(_WIN32)
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
        return std::unexpected{axk::make_error(axk::ErrorCode::io_unsupported_size, axk::ErrorCategory::io,
                                               "sandbox mutation offset exceeds the platform range")};
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (SetFilePointerEx(implementation_->file.get(), position, nullptr, FILE_BEGIN) == 0)
        return std::unexpected{
            axk::make_error(axk::ErrorCode::io_read_failed, axk::ErrorCategory::io, "sandbox mutation seek failed")};
    while (!remaining.empty()) {
        const auto chunk =
            static_cast<DWORD>(std::min<std::size_t>(remaining.size(), std::numeric_limits<DWORD>::max()));
        DWORD read{};
        if (ReadFile(implementation_->file.get(), remaining.data(), chunk, &read, nullptr) == 0 || read == 0U)
            return std::unexpected{axk::make_error(axk::ErrorCode::io_read_failed, axk::ErrorCategory::io,
                                                   "sandbox mutation read failed")};
        remaining = remaining.subspan(static_cast<std::size_t>(read));
    }
#else
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return std::unexpected{axk::make_error(axk::ErrorCode::io_unsupported_size, axk::ErrorCategory::io,
                                               "sandbox mutation offset exceeds the platform range")};
    }
    auto position = static_cast<off_t>(offset);
    while (!remaining.empty()) {
        const auto read = ::pread(*implementation_->file, remaining.data(), remaining.size(), position);
        if (read < 0 && errno == EINTR)
            continue;
        if (read <= 0)
            return std::unexpected{axk::make_error(axk::ErrorCode::io_read_failed, axk::ErrorCategory::io,
                                                   "sandbox mutation read failed")};
        remaining = remaining.subspan(static_cast<std::size_t>(read));
        position += read;
    }
#endif
    return {};
}

axk::app::Result<void> axk::app::SandboxMutation::write_exact_at(std::uint64_t offset,
                                                                 std::span<const std::byte> source) {
    if (!implementation_ || offset > size() || source.size() > size() - offset)
        return std::unexpected(entry_error("entry_mutation_failed", "sandbox mutation exceeds the file", {}));
    const std::scoped_lock lock{implementation_->io_mutex};
    auto remaining = source;
#if defined(_WIN32)
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()))
        return std::unexpected(entry_error("entry_mutation_failed", "sandbox mutation offset is unsupported", {}));
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (SetFilePointerEx(implementation_->file.get(), position, nullptr, FILE_BEGIN) == 0)
        return std::unexpected(entry_error("entry_mutation_failed", "sandbox mutation seek failed", {}));
    while (!remaining.empty()) {
        const auto chunk =
            static_cast<DWORD>(std::min<std::size_t>(remaining.size(), std::numeric_limits<DWORD>::max()));
        DWORD written{};
        if (WriteFile(implementation_->file.get(), remaining.data(), chunk, &written, nullptr) == 0 || written == 0U)
            return std::unexpected(entry_error("entry_mutation_failed", "sandbox mutation write failed", {}));
        remaining = remaining.subspan(static_cast<std::size_t>(written));
    }
#else
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
        return std::unexpected(entry_error("entry_mutation_failed", "sandbox mutation offset is unsupported", {}));
    auto position = static_cast<off_t>(offset);
    while (!remaining.empty()) {
        const auto written = ::pwrite(*implementation_->file, remaining.data(), remaining.size(), position);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return std::unexpected(entry_error("entry_mutation_failed", "sandbox mutation write failed", {}));
        remaining = remaining.subspan(static_cast<std::size_t>(written));
        position += written;
    }
#endif
    return {};
}

axk::app::Result<void> axk::app::SandboxMutation::flush() {
    if (!implementation_)
        return std::unexpected(entry_error("entry_mutation_failed", "sandbox mutation is closed", {}));
#if defined(_WIN32)
    if (FlushFileBuffers(implementation_->file.get()) == 0)
#else
    if (::fsync(*implementation_->file) != 0)
#endif
        return std::unexpected(entry_error("entry_mutation_failed", "sandbox mutation could not be flushed",
                                           implementation_->reference.relative_path));
    return {};
}

axk::app::Result<void> axk::app::SandboxMutation::verify_bound() const {
    if (!implementation_)
        return std::unexpected(entry_error("entry_mutation_failed", "sandbox mutation is closed", {}));
#if defined(_WIN32)
    auto current = open_relative(implementation_->parent.get(), implementation_->filename, FILE_READ_ATTRIBUTES,
                                 FILE_OPEN, FILE_NON_DIRECTORY_FILE, implementation_->reference.relative_path);
    if (!current)
        return std::unexpected(entry_error("entry_mutation_failed", "sandbox mutation target changed",
                                           implementation_->reference.relative_path));
    auto identity = native_identity(current->get(), implementation_->reference.relative_path);
    const auto same = identity && identity->volume_serial == implementation_->identity.volume_serial &&
                      identity->file_id == implementation_->identity.file_id;
#else
    struct stat status{};
    const auto same =
        ::fstatat(*implementation_->parent, implementation_->filename.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISREG(status.st_mode) && static_cast<std::uint64_t>(status.st_dev) == implementation_->identity.device &&
        static_cast<std::uint64_t>(status.st_ino) == implementation_->identity.inode;
#endif
    if (!same)
        return std::unexpected(entry_error("entry_mutation_failed", "sandbox mutation target changed",
                                           implementation_->reference.relative_path));
    return {};
}

axk::app::SandboxTree::SandboxTree() = default;
axk::app::SandboxTree::~SandboxTree() = default;
axk::app::SandboxTree::SandboxTree(SandboxTree &&) noexcept = default;
axk::app::SandboxTree &axk::app::SandboxTree::operator=(SandboxTree &&) noexcept = default;

axk::app::SandboxTree::SandboxTree(std::unique_ptr<Implementation> implementation)
    : implementation_(std::move(implementation)) {}

std::span<const axk::app::SandboxTreeEntry> axk::app::SandboxTree::entries() const noexcept {
    return implementation_ ? std::span<const SandboxTreeEntry>{implementation_->entries}
                           : std::span<const SandboxTreeEntry>{};
}

axk::app::Result<axk::app::OpenedSandboxTreeFile> axk::app::SandboxTree::open_file(std::size_t index) const {
    if (!implementation_ || index >= implementation_->entries.size() ||
        implementation_->entries[index].kind != SandboxTreeEntryKind::file) {
        return std::unexpected(entry_error("archive_source_changed", "archive file entry is unavailable", {}));
    }
    const auto &entry = implementation_->entries[index];
    auto relative = relative_path_from_utf8(entry.relative_path);
    if (!relative || relative->filename().empty())
        return std::unexpected(
            entry_error("archive_source_changed", "archive file path is invalid", entry.relative_path));
#if defined(_WIN32)
    auto parent = open_parent(implementation_->root.get(), relative->parent_path(), entry.relative_path);
#else
    auto parent = open_parent(*implementation_->root, relative->parent_path(), entry.relative_path);
#endif
    if (!parent)
        return std::unexpected(entry_error("archive_source_changed", "archive parent changed", entry.relative_path));
#if defined(_WIN32)
    auto opened = open_relative(parent->get(), relative->filename(), FILE_READ_DATA | FILE_READ_ATTRIBUTES, FILE_OPEN,
                                FILE_NON_DIRECTORY_FILE, entry.relative_path);
    if (!opened)
        return std::unexpected(entry_error("archive_source_changed", "archive file changed", entry.relative_path));
    auto file_handle = std::move(*opened);
    auto identity = native_identity(file_handle.get(), entry.relative_path);
#else
    const auto descriptor =
        ::openat(**parent, relative->filename().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0)
        return std::unexpected(entry_error("archive_source_changed", "archive file changed", entry.relative_path));
    auto file_handle = descriptor_handle(descriptor);
    auto identity = native_identity(*file_handle, entry.relative_path);
#endif
    if (!identity || !same_file_revision(*identity, implementation_->identities[index]))
        return std::unexpected(entry_error("archive_source_changed", "archive file changed", entry.relative_path));
    auto reader = std::make_shared<NativeFileReader>(std::move(file_handle), entry.size, entry.relative_path);
    const auto expected = implementation_->identities[index];
    return OpenedSandboxTreeFile{reader, [reader, expected] { return reader->verify_unchanged(expected); }};
}
