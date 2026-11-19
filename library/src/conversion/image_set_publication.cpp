#include "axklib/conversion/image_set_publication.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <stdio.h>
#endif
#endif

namespace axk::conversion {
namespace {
namespace fs = std::filesystem;
// Keep an open directory handle as the reference. A path that now names a different
// object must not be used for publication or cleanup.
class StageIdentity {
  public:
    explicit StageIdentity(const fs::path &path)
#ifdef _WIN32
        : handle_(open_directory(path)) {
    }
    ~StageIdentity() {
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
    }
    bool matches(const fs::path &path) const {
        if (handle_ == INVALID_HANDLE_VALUE)
            return false;
        BY_HANDLE_FILE_INFORMATION original{}, current{};
        HANDLE candidate = open_directory(path);
        if (candidate == INVALID_HANDLE_VALUE)
            return false;
        const bool okay =
            GetFileInformationByHandle(handle_, &original) && GetFileInformationByHandle(candidate, &current) &&
            (current.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(current.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            original.dwVolumeSerialNumber == current.dwVolumeSerialNumber &&
            original.nFileIndexHigh == current.nFileIndexHigh && original.nFileIndexLow == current.nFileIndexLow;
        CloseHandle(candidate);
        return okay;
    }

  private:
    static HANDLE open_directory(const fs::path &path) {
        return CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    }
    HANDLE handle_;
#else
        : fd_(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)) {
    }
    ~StageIdentity() {
        if (fd_ >= 0)
            ::close(fd_);
    }
    bool matches(const fs::path &path) const {
        struct stat original{}, current{};
        return fd_ >= 0 && ::fstat(fd_, &original) == 0 && ::lstat(path.c_str(), &current) == 0 &&
               S_ISDIR(current.st_mode) && original.st_dev == current.st_dev && original.st_ino == current.st_ino;
    }

  private:
    int fd_;
#endif
    StageIdentity(const StageIdentity &) = delete;
    StageIdentity &operator=(const StageIdentity &) = delete;
};

Error failure(std::string message) {
    return make_error(ErrorCode::io_open_failed, ErrorCategory::io, std::move(message));
}
Result<void> write_file(const fs::path &path, std::span<const std::byte> data, const CancellationToken &cancel) {
#ifdef _WIN32
    std::FILE *raw{};
    if (_wfopen_s(&raw, path.c_str(), L"wbx") != 0)
        raw = nullptr;
#else
    auto *raw = std::fopen(path.c_str(), "wbx");
#endif
    if (!raw)
        return std::unexpected(failure("Could not create image-set member"));
    std::unique_ptr<std::FILE, decltype(&std::fclose)> output(raw, &std::fclose);
    while (!data.empty()) {
        if (auto ok = cancel.check(); !ok)
            return ok;
        const auto chunk = data.first(std::min<std::size_t>(data.size(), 1024U * 1024U));
        if (std::fwrite(chunk.data(), 1, chunk.size(), output.get()) != chunk.size())
            return std::unexpected(failure("Could not write complete image-set member"));
        data = data.subspan(chunk.size());
    }
    if (std::fflush(output.get()) != 0)
        return std::unexpected(failure("Could not flush image-set member"));
#ifdef _WIN32
    const int synchronized = _commit(_fileno(output.get()));
#else
    const int synchronized = ::fsync(::fileno(output.get()));
#endif
    if (synchronized != 0 || std::fclose(output.release()) != 0)
        return std::unexpected(failure("Could not synchronize image-set member"));
    return {};
}

Result<fs::path> private_directory(const fs::path &parent) {
#ifdef _WIN32
    std::random_device random;
    for (unsigned n = 0; n != 32; ++n) {
        const auto path = parent / (".axk-images-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (CreateDirectoryW(path.c_str(), nullptr))
            return path;
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            break;
    }
#else
    auto text = (parent / ".axk-images-XXXXXX").native();
    if (::mkdtemp(text.data()))
        return fs::path{text};
#endif
    return std::unexpected(failure("Could not create temporary image set beside output"));
}

bool rename_exclusive(const fs::path &stage, const fs::path &destination) {
#ifdef _WIN32
    return MoveFileExW(stage.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
#elif defined(__linux__)
    constexpr unsigned no_replace = 1U;
    return ::syscall(SYS_renameat2, AT_FDCWD, stage.c_str(), AT_FDCWD, destination.c_str(), no_replace) == 0;
#elif defined(__APPLE__)
    return ::renamex_np(stage.c_str(), destination.c_str(), RENAME_EXCL) == 0;
#else
    return false;
#endif
}
#ifndef _WIN32
bool sync_directory(const fs::path &path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}
#endif
} // namespace

Result<void> stage_image_set(const PreparedPlan &plan, const std::filesystem::path &staging,
                             const CancellationToken &cancel, ProgressSink *progress) {
    const auto *images = std::get_if<NewImageSet>(&plan.output());
    if (!plan.report().executable || !images || images->members.empty())
        return std::unexpected(failure("Expected an executable image-set plan"));
    if (auto valid = images->validate(plan.retained_bytes()); !valid)
        return valid;
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(staging, ec)) || ec || !fs::is_empty(staging, ec) || ec)
        return std::unexpected(failure("Image-set staging directory must be empty"));
    const StageIdentity identity(staging);
    unsigned number = 0;
    for (const auto &member : images->members) {
        if (!identity.matches(staging))
            return std::unexpected(failure("Image-set staging directory has changed"));
        if (auto ok = write_file(staging / member.filename, member.bytes, cancel); !ok)
            return ok;
        if (progress)
            progress->report({ProgressPhase::writing, ++number, images->members.size(), "Writing disk images", {}});
    }
    auto manifest = nlohmann::json(std::get<NewImageSetReview>(plan.report().destination));
    manifest["profileId"] = plan.report().profile_id;
    const auto text = manifest.dump(2) + "\n";
    if (!identity.matches(staging))
        return std::unexpected(failure("Image-set staging directory has changed"));
    return write_file(staging / "manifest.json", std::as_bytes(std::span{text.data(), text.size()}), cancel);
}

Result<PublicationOutcome> publish_image_set(const PreparedPlan &plan, const fs::path &destination,
                                             const std::function<Result<void>()> &verify_source,
                                             const CancellationToken &cancel, ProgressSink *progress) {
    const auto leaf = destination.filename();
    if (leaf.empty() || leaf == "." || leaf == "..")
        return std::unexpected(failure("Image set requires a new directory name"));
    const auto *images = std::get_if<NewImageSet>(&plan.output());
    if (!images)
        return std::unexpected(failure("Expected an image-set plan"));
    if (auto valid = images->validate(plan.retained_bytes()); !valid)
        return std::unexpected(valid.error());
    std::error_code error;
    const auto parent = fs::canonical(destination.has_parent_path() ? destination.parent_path() : fs::path{"."}, error);
    if (error)
        return std::unexpected(failure("Image-set parent is unavailable"));
    auto stage = private_directory(parent);
    if (!stage)
        return std::unexpected(stage.error());
    const StageIdentity identity(*stage);
    struct Cleanup {
        fs::path path;
        const StageIdentity &identity;
        const NewImageSet &images;
        ~Cleanup() {
            if (!path.empty() && identity.matches(path)) {
                std::error_code ignored;
                for (const auto &member : images.members)
                    fs::remove(path / member.filename, ignored);
                fs::remove(path / "manifest.json", ignored);
                fs::remove(path, ignored);
            }
        }
    } cleanup{*stage, identity, *images};
    if (auto ok = stage_image_set(plan, *stage, cancel, progress); !ok)
        return std::unexpected(ok.error());
#ifndef _WIN32
    if (!sync_directory(*stage))
        return std::unexpected(failure("Could not synchronize staged image set"));
#endif
    if (progress)
        progress->report({ProgressPhase::publishing, 0, 1, "Publishing image set", {}});
    if (verify_source) {
        if (auto current = verify_source(); !current)
            return std::unexpected(current.error());
    }
    if (auto active = cancel.check(); !active)
        return std::unexpected(active.error());
    if (!identity.matches(*stage))
        return std::unexpected(failure("Image-set staging directory has changed"));
    if (!rename_exclusive(*stage, parent / leaf))
        return std::unexpected(failure("Could not publish image set; destination must not exist"));
    cleanup.path.clear();
    PublicationOutcome result;
#ifndef _WIN32
    if (!sync_directory(parent)) {
        result.durability = PublicationDurability::unconfirmed;
        result.warnings.push_back(
            {"publication_durability_unconfirmed", "Directory synchronization failed after publication"});
    }
#endif
    return result;
}
} // namespace axk::conversion
