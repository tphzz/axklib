#include "axklib/application/uploads.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "axklib/application/secure_random.hpp"
#include "axklib/io.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/utf8.hpp"
#include "private_storage.hpp"

namespace {

axk::app::Error upload_error(std::string code, std::string message, bool retryable = false) {
    return {std::move(code), std::move(message), {}, retryable};
}

std::string lowercase(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

bool valid_digest(const std::optional<std::string> &digest) {
    return !digest ||
           (digest->size() == 64U && std::ranges::all_of(*digest, [](unsigned char character) {
                return std::isxdigit(character) != 0 && (std::isdigit(character) != 0 || std::islower(character) != 0);
            }));
}

bool admitted_extension(axk::app::UploadKind kind, const std::filesystem::path &path) {
    const auto extension = lowercase(axk::text::path_to_utf8(path.extension()));
    constexpr std::array audio_extensions{".wav", ".wave", ".flac", ".aif", ".aiff"};
    constexpr std::array package_extensions{".axkpkg", ".axkvol", ".axkprg", ".axksbac", ".axksbnk", ".axksmpl"};
    switch (kind) {
    case axk::app::UploadKind::audio:
        return std::ranges::find(audio_extensions, extension) != audio_extensions.end();
    case axk::app::UploadKind::package:
        return std::ranges::find(package_extensions, extension) != package_extensions.end();
    case axk::app::UploadKind::manifest:
        return extension == ".json";
    }
    return false;
}

bool valid_media_type(axk::app::UploadKind kind, std::string_view value) {
    switch (kind) {
    case axk::app::UploadKind::audio:
        return value == "audio/wav" || value == "audio/x-wav" || value == "audio/flac" || value == "audio/aiff" ||
               value == "audio/x-aiff" || value == "application/octet-stream";
    case axk::app::UploadKind::package:
        return value == "application/vnd.axklib.package" || value == "application/octet-stream";
    case axk::app::UploadKind::manifest:
        return value == "application/json";
    }
    return false;
}

std::string_view disallowed_upload_message(axk::app::UploadKind kind) {
    switch (kind) {
    case axk::app::UploadKind::audio:
        return "audio uploads require a WAV, FLAC, or AIFF file";
    case axk::app::UploadKind::package:
        return "package uploads require an axklib package file";
    case axk::app::UploadKind::manifest:
        return "manifest uploads require a JSON file";
    }
    return "upload type is not allowed";
}

class UploadFile {
  public:
    UploadFile(const UploadFile &) = delete;
    UploadFile &operator=(const UploadFile &) = delete;
    ~UploadFile() {
#if defined(_WIN32)
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
#else
        if (descriptor_ >= 0)
            ::close(descriptor_);
#endif
    }

    static axk::app::Result<std::shared_ptr<UploadFile>> open(const std::filesystem::path &path) {
#if defined(_WIN32)
        const auto handle =
            CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return std::unexpected(upload_error("upload_storage_unavailable", "upload staging file cannot be opened"));
        return std::shared_ptr<UploadFile>{new UploadFile(handle)};
#else
        const auto descriptor = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor < 0)
            return std::unexpected(upload_error("upload_storage_unavailable", "upload staging file cannot be opened"));
        return std::shared_ptr<UploadFile>{new UploadFile(descriptor)};
#endif
    }

    axk::app::Result<void> write_at(std::uint64_t offset, std::span<const std::byte> bytes) {
#if defined(_WIN32)
        LARGE_INTEGER position{.QuadPart = static_cast<LONGLONG>(offset)};
        if (SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN) == 0)
            return std::unexpected(upload_error("upload_storage_unavailable", "upload chunk cannot be positioned"));
        std::size_t completed{};
        while (completed < bytes.size()) {
            const auto count =
                static_cast<DWORD>(std::min<std::size_t>(bytes.size() - completed, std::numeric_limits<DWORD>::max()));
            DWORD written{};
            if (WriteFile(handle_, bytes.data() + completed, count, &written, nullptr) == 0 || written == 0U) {
                truncate(offset);
                return std::unexpected(upload_error("upload_storage_unavailable", "upload chunk cannot be stored"));
            }
            completed += written;
        }
#else
        std::size_t completed{};
        while (completed < bytes.size()) {
            const auto written = ::pwrite(descriptor_, bytes.data() + completed, bytes.size() - completed,
                                          static_cast<off_t>(offset + completed));
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0) {
                truncate(offset);
                return std::unexpected(upload_error("upload_storage_unavailable", "upload chunk cannot be stored"));
            }
            completed += static_cast<std::size_t>(written);
        }
#endif
        auto physical_size = size();
        if (!physical_size || *physical_size != offset + bytes.size()) {
            truncate(offset);
            return std::unexpected(upload_error("upload_storage_unavailable", "upload chunk size cannot be verified"));
        }
        return {};
    }

    axk::app::Result<std::uint64_t> size() const {
#if defined(_WIN32)
        LARGE_INTEGER value{};
        if (GetFileSizeEx(handle_, &value) == 0 || value.QuadPart < 0)
            return std::unexpected(upload_error("upload_storage_unavailable", "upload size cannot be inspected"));
        return static_cast<std::uint64_t>(value.QuadPart);
#else
        struct stat status{};
        if (::fstat(descriptor_, &status) != 0 || status.st_size < 0)
            return std::unexpected(upload_error("upload_storage_unavailable", "upload size cannot be inspected"));
        return static_cast<std::uint64_t>(status.st_size);
#endif
    }

    axk::app::Result<void> flush() const {
#if defined(_WIN32)
        if (FlushFileBuffers(handle_) == 0)
#else
        if (::fsync(descriptor_) != 0)
#endif
            return std::unexpected(upload_error("upload_storage_unavailable", "upload cannot be synchronized"));
        return {};
    }

  private:
#if defined(_WIN32)
    explicit UploadFile(HANDLE handle) : handle_(handle) {}
    void truncate(std::uint64_t size) {
        LARGE_INTEGER position{.QuadPart = static_cast<LONGLONG>(size)};
        if (SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN) != 0)
            static_cast<void>(SetEndOfFile(handle_));
    }
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    explicit UploadFile(int descriptor) : descriptor_(descriptor) {}
    void truncate(std::uint64_t size) { static_cast<void>(::ftruncate(descriptor_, static_cast<off_t>(size))); }
    int descriptor_{-1};
#endif
};

} // namespace

struct axk::app::UploadStore::Implementation {
    struct Entry {
        UploadCreateRequest request;
        std::filesystem::path path;
        std::shared_ptr<UploadFile> file;
        std::uint64_t received_size{};
        UploadState state{UploadState::receiving};
        std::chrono::steady_clock::time_point expires_at;
        std::size_t lease_count{};
    };

    std::filesystem::path staging_directory;
    std::uint64_t maximum_total_bytes{};
    std::uint64_t maximum_upload_bytes{};
    std::size_t maximum_uploads{};
    std::size_t maximum_chunk_bytes{};
    std::chrono::seconds retention{};
    Clock clock;
    RemoveFile remove_file;
    std::uint64_t reserved_bytes{};
    std::uint64_t failed_deletions{};
    bool startup_scan_failed{};
    std::mutex mutex;
    std::unordered_map<std::string, Entry> entries;
    std::unordered_map<std::filesystem::path, std::uint64_t> orphan_files;

    Implementation(std::filesystem::path directory, std::uint64_t total_bytes, std::uint64_t upload_bytes,
                   std::size_t upload_count, std::size_t chunk_bytes, std::chrono::seconds upload_retention,
                   Clock upload_clock, RemoveFile upload_remove_file)
        : staging_directory(std::move(directory)), maximum_total_bytes(total_bytes), maximum_upload_bytes(upload_bytes),
          maximum_uploads(upload_count), maximum_chunk_bytes(chunk_bytes), retention(upload_retention),
          clock(std::move(upload_clock)), remove_file(std::move(upload_remove_file)) {
        if (!remove_file) {
            remove_file = [](const std::filesystem::path &path, std::error_code &error) {
                return std::filesystem::remove(path, error);
            };
        }
    }

    [[nodiscard]] UploadSnapshot snapshot(std::string_view id, const Entry &entry) const {
        const auto remaining =
            entry.expires_at > clock()
                ? std::chrono::duration_cast<std::chrono::seconds>(entry.expires_at - clock()).count()
                : 0;
        return {.reference = {std::string{id}},
                .filename = entry.request.filename,
                .kind = entry.request.kind,
                .media_type = entry.request.media_type,
                .declared_size = entry.request.declared_size,
                .received_size = entry.received_size,
                .state = entry.state,
                .expires_in_seconds = static_cast<std::uint64_t>(remaining)};
    }

    void cleanup_locked() {
        failed_deletions = 0U;
        for (auto iterator = orphan_files.begin(); iterator != orphan_files.end();) {
            std::error_code error;
            const auto removed = remove_file(iterator->first, error);
            if (!removed && error) {
                ++failed_deletions;
                ++iterator;
                continue;
            }
            reserved_bytes -= iterator->second;
            iterator = orphan_files.erase(iterator);
        }
        const auto now = clock();
        for (auto iterator = entries.begin(); iterator != entries.end();) {
            if (iterator->second.expires_at > now || iterator->second.lease_count != 0U) {
                ++iterator;
                continue;
            }
            std::error_code error;
            const auto removed = remove_file(iterator->second.path, error);
            if (!removed && error) {
                ++failed_deletions;
                ++iterator;
                continue;
            }
            reserved_bytes -= iterator->second.request.declared_size;
            iterator = entries.erase(iterator);
        }
    }

    [[nodiscard]] UploadCleanupSnapshot cleanup_snapshot_locked() const {
        std::uint64_t orphan_bytes{};
        for (const auto &[path, size] : orphan_files) {
            static_cast<void>(path);
            orphan_bytes += size;
        }
        return {.healthy = !startup_scan_failed && failed_deletions == 0U && orphan_files.empty(),
                .failed_deletions = failed_deletions,
                .orphan_count = orphan_files.size(),
                .orphan_bytes = orphan_bytes,
                .reserved_bytes = reserved_bytes};
    }

    [[nodiscard]] Result<std::unordered_map<std::string, Entry>::iterator> owned(const UploadRef &reference,
                                                                                 std::string_view owner_id) {
        cleanup_locked();
        const auto found = entries.find(reference.upload_id);
        if (found == entries.end() || found->second.request.owner_id != owner_id)
            return std::unexpected(upload_error("upload_not_found", "upload does not exist"));
        return found;
    }
};

axk::app::UploadStore::UploadStore(std::filesystem::path staging_directory, std::uint64_t maximum_total_bytes,
                                   std::uint64_t maximum_upload_bytes, std::size_t maximum_uploads,
                                   std::size_t maximum_chunk_bytes, std::chrono::seconds retention, Clock clock,
                                   RemoveFile remove_file)
    : implementation_(std::make_shared<Implementation>(std::move(staging_directory), maximum_total_bytes,
                                                       maximum_upload_bytes, maximum_uploads, maximum_chunk_bytes,
                                                       retention, std::move(clock), std::move(remove_file))) {
    std::error_code error;
    const auto prepared = detail::prepare_private_directory(implementation_->staging_directory);
    if (prepared) {
        for (const auto &entry : std::filesystem::directory_iterator{implementation_->staging_directory, error}) {
            if (error)
                break;
            if (entry.is_regular_file(error) && entry.path().extension() == ".upload") {
                const auto size = entry.file_size(error);
                if (error)
                    break;
                const auto removed = implementation_->remove_file(entry.path(), error);
                if (!removed && error) {
                    implementation_->orphan_files.emplace(entry.path(), size);
                    implementation_->reserved_bytes += size;
                    error.clear();
                }
            }
            if (error)
                break;
        }
    }
    implementation_->startup_scan_failed = !prepared || static_cast<bool>(error);
    implementation_->failed_deletions = implementation_->orphan_files.size();
}

axk::app::UploadStore::~UploadStore() = default;
axk::app::UploadStore::UploadStore(UploadStore &&) noexcept = default;
axk::app::UploadStore &axk::app::UploadStore::operator=(UploadStore &&) noexcept = default;

axk::app::Result<axk::app::UploadSnapshot> axk::app::UploadStore::create(UploadCreateRequest request) {
    if (request.owner_id.empty() || request.filename.empty() || request.filename.size() > 255U ||
        request.declared_size == 0U || request.declared_size > implementation_->maximum_upload_bytes ||
        !valid_digest(request.sha256)) {
        return std::unexpected(upload_error("invalid_upload", "upload metadata or declared size is invalid"));
    }
    auto filename = text::path_from_utf8(request.filename);
    if (!filename || filename->filename() != *filename || !admitted_extension(request.kind, *filename) ||
        !valid_media_type(request.kind, request.media_type)) {
        return std::unexpected(
            upload_error("upload_type_not_allowed", std::string{disallowed_upload_message(request.kind)}));
    }

    const std::scoped_lock lock{implementation_->mutex};
    if (implementation_->startup_scan_failed)
        return std::unexpected(upload_error("upload_storage_unavailable", "upload staging directory is not private"));
    implementation_->cleanup_locked();
    if (implementation_->entries.size() >= implementation_->maximum_uploads ||
        implementation_->reserved_bytes >= implementation_->maximum_total_bytes ||
        request.declared_size > implementation_->maximum_total_bytes - implementation_->reserved_bytes) {
        return std::unexpected(upload_error("upload_quota_exceeded", "upload staging quota is exhausted", true));
    }
    std::string id;
    do {
        auto generated = secure_random_hex(32U);
        if (!generated)
            return std::unexpected(upload_error("upload_storage_unavailable", generated.error().message));
        id = std::move(*generated);
    } while (implementation_->entries.contains(id));
    const auto path = implementation_->staging_directory / (id + ".upload");
    if (!detail::create_private_file(path))
        return std::unexpected(upload_error("upload_storage_unavailable", "upload staging file cannot be created"));
    auto file = UploadFile::open(path);
    if (!file) {
        std::error_code cleanup_error;
        std::filesystem::remove(path, cleanup_error);
        return std::unexpected(file.error());
    }
    auto [position, inserted] = implementation_->entries.emplace(
        id, Implementation::Entry{.request = std::move(request),
                                  .path = path,
                                  .file = std::move(*file),
                                  .received_size = 0U,
                                  .state = UploadState::receiving,
                                  .expires_at = implementation_->clock() + implementation_->retention,
                                  .lease_count = 0U});
    if (!inserted)
        return std::unexpected(upload_error("upload_storage_unavailable", "upload ID collision"));
    implementation_->reserved_bytes += position->second.request.declared_size;
    return implementation_->snapshot(position->first, position->second);
}

axk::app::Result<axk::app::UploadSnapshot> axk::app::UploadStore::append(const UploadRef &reference,
                                                                         std::string_view owner_id,
                                                                         std::uint64_t offset,
                                                                         std::span<const std::byte> bytes) {
    if (bytes.empty() || bytes.size() > implementation_->maximum_chunk_bytes)
        return std::unexpected(upload_error("invalid_upload_chunk", "upload chunk size is invalid"));
    const std::scoped_lock lock{implementation_->mutex};
    auto found = implementation_->owned(reference, owner_id);
    if (!found)
        return std::unexpected(found.error());
    auto &entry = (**found).second;
    if (entry.state != UploadState::receiving || offset != entry.received_size ||
        bytes.size() > entry.request.declared_size - entry.received_size) {
        return std::unexpected(upload_error("invalid_upload_chunk", "upload chunk offset or state is invalid"));
    }
    if (auto written = entry.file->write_at(entry.received_size, bytes); !written)
        return std::unexpected(written.error());
    entry.received_size += bytes.size();
    entry.expires_at = implementation_->clock() + implementation_->retention;
    return implementation_->snapshot(reference.upload_id, entry);
}

axk::app::Result<axk::app::UploadSnapshot> axk::app::UploadStore::complete(const UploadRef &reference,
                                                                           std::string_view owner_id) {
    const std::scoped_lock lock{implementation_->mutex};
    auto found = implementation_->owned(reference, owner_id);
    if (!found)
        return std::unexpected(found.error());
    auto &entry = (**found).second;
    if (entry.received_size != entry.request.declared_size)
        return std::unexpected(upload_error("upload_incomplete", "upload has not received its declared size"));
    const auto physical_size = entry.file->size();
    if (!physical_size)
        return std::unexpected(physical_size.error());
    if (*physical_size != entry.request.declared_size)
        return std::unexpected(
            upload_error("upload_incomplete", "upload physical size does not match its declaration"));
    if (auto flushed = entry.file->flush(); !flushed)
        return std::unexpected(flushed.error());
    if (entry.request.sha256) {
        const auto reader = FileReader::open(entry.path);
        if (!reader)
            return std::unexpected(upload_error("upload_storage_unavailable", "upload cannot be reopened"));
        const auto digest = package_internal::sha256_reader(**reader);
        if (!digest || package_internal::hex_digest(*digest) != *entry.request.sha256)
            return std::unexpected(upload_error("upload_hash_mismatch", "upload SHA-256 does not match"));
    }
    entry.state = UploadState::ready;
    entry.expires_at = implementation_->clock() + implementation_->retention;
    return implementation_->snapshot(reference.upload_id, entry);
}

axk::app::Result<axk::app::UploadSnapshot> axk::app::UploadStore::inspect(const UploadRef &reference,
                                                                          std::string_view owner_id) {
    const std::scoped_lock lock{implementation_->mutex};
    auto found = implementation_->owned(reference, owner_id);
    if (!found)
        return std::unexpected(found.error());
    return implementation_->snapshot(reference.upload_id, (**found).second);
}

axk::app::Result<std::filesystem::path> axk::app::UploadStore::resolve(const UploadRef &reference,
                                                                       std::string_view owner_id) {
    const std::scoped_lock lock{implementation_->mutex};
    auto found = implementation_->owned(reference, owner_id);
    if (!found)
        return std::unexpected(found.error());
    if ((**found).second.state != UploadState::ready)
        return std::unexpected(upload_error("upload_not_ready", "upload is not finalized"));
    (**found).second.expires_at = implementation_->clock() + implementation_->retention;
    return (**found).second.path;
}

axk::app::Result<axk::app::UploadLease> axk::app::UploadStore::lease(const UploadRef &reference,
                                                                     std::string_view owner_id) {
    std::filesystem::path path;
    {
        const std::scoped_lock lock{implementation_->mutex};
        auto found = implementation_->owned(reference, owner_id);
        if (!found)
            return std::unexpected(found.error());
        auto &entry = (**found).second;
        if (entry.state != UploadState::ready)
            return std::unexpected(upload_error("upload_not_ready", "upload is not finalized"));
        ++entry.lease_count;
        entry.expires_at = implementation_->clock() + implementation_->retention;
        path = entry.path;
    }
    auto implementation = implementation_;
    auto guard = std::shared_ptr<void>{new std::byte{}, [implementation, id = reference.upload_id](void *value) {
                                           delete static_cast<std::byte *>(value);
                                           const std::scoped_lock lock{implementation->mutex};
                                           const auto found = implementation->entries.find(id);
                                           if (found == implementation->entries.end())
                                               return;
                                           if (found->second.lease_count != 0U)
                                               --found->second.lease_count;
                                           found->second.expires_at =
                                               implementation->clock() + implementation->retention;
                                       }};
    UploadLease result;
    result.path_ = std::move(path);
    result.guard_ = std::move(guard);
    return result;
}

axk::app::Result<axk::app::FileRef> axk::app::UploadStore::materialize(const UploadRef &reference,
                                                                       std::string_view owner_id,
                                                                       const Sandbox &sandbox,
                                                                       const FileRef &destination, bool overwrite) {
    auto retained = lease(reference, owner_id);
    if (!retained)
        return std::unexpected(retained.error());
    const auto input = axk::FileReader::open(retained->path());
    if (!input)
        return std::unexpected(upload_error("upload_materialization_failed", input.error().message));
    if (const auto published = sandbox.publish_file(destination, overwrite, **input); !published)
        return std::unexpected(published.error());
    return destination;
}

axk::app::Result<void> axk::app::UploadStore::remove(const UploadRef &reference, std::string_view owner_id) {
    const std::scoped_lock lock{implementation_->mutex};
    auto found = implementation_->owned(reference, owner_id);
    if (!found)
        return std::unexpected(found.error());
    if ((**found).second.lease_count != 0U)
        return std::unexpected(upload_error("upload_in_use", "upload is retained by an active operation"));
    std::error_code error;
    const auto removed = implementation_->remove_file((**found).second.path, error);
    if (!removed && error)
        return std::unexpected(upload_error("upload_storage_unavailable", "upload staging file cannot be removed"));
    implementation_->reserved_bytes -= (**found).second.request.declared_size;
    implementation_->entries.erase(*found);
    return {};
}

void axk::app::UploadStore::cleanup() {
    const std::scoped_lock lock{implementation_->mutex};
    implementation_->cleanup_locked();
}

axk::app::UploadCleanupSnapshot axk::app::UploadStore::cleanup_snapshot() {
    const std::scoped_lock lock{implementation_->mutex};
    implementation_->cleanup_locked();
    return implementation_->cleanup_snapshot_locked();
}

bool axk::app::UploadStore::storage_ready() const noexcept {
    return implementation_ != nullptr && !implementation_->startup_scan_failed;
}

std::size_t axk::app::UploadStore::maximum_chunk_bytes() const noexcept { return implementation_->maximum_chunk_bytes; }

std::string_view axk::app::upload_kind_name(UploadKind kind) noexcept {
    switch (kind) {
    case UploadKind::audio:
        return "AUDIO";
    case UploadKind::package:
        return "PACKAGE";
    case UploadKind::manifest:
        return "MANIFEST";
    }
    return "AUDIO";
}

std::string_view axk::app::upload_state_name(UploadState state) noexcept {
    switch (state) {
    case UploadState::receiving:
        return "RECEIVING";
    case UploadState::ready:
        return "READY";
    }
    return "RECEIVING";
}
