#include "axklib/inspection/disk.hpp"

#include <filesystem>
#include <utility>

#include "axklib/filesystems/fat/view.hpp"
#include "axklib/formats/tx16w/document.hpp"
#include "axklib/formats/typhoon/document.hpp"
#include "axklib/media.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/sfs_disk_sources.hpp"
#include "axklib/utf8.hpp"

namespace axk::inspection {
using namespace native;
namespace {
struct FileInput {
    std::filesystem::path path;
    package_internal::Sha256Digest digest;
    std::uint64_t size;
};

Error invalid(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::io, std::move(message));
}
} // namespace

Result<DiskInspection> DiskInspection::open(std::span<const std::filesystem::path> paths,
                                            const CancellationToken &cancellation) {
    if (paths.empty() || paths.size() > SourceSet::maximum_members)
        return std::unexpected(invalid("Select one to 32 explicit source disk images"));
    std::vector<SourceMember> members;
    std::vector<FileInput> files;
    SfsDiskSources sfs;
    std::uint64_t remaining_bytes = 8ULL * 1024U * 1024U * 1024U;
    for (const auto &path : paths) {
        std::error_code error;
        const auto canonical = std::filesystem::canonical(path, error);
        if (error || !std::filesystem::is_regular_file(canonical, error) || error)
            return std::unexpected(invalid("Each source must be a readable disk image file"));
        for (const auto &other : files) {
            if (std::filesystem::equivalent(canonical, other.path, error) || error)
                return std::unexpected(invalid("Cannot admit duplicate or unverifiable disk members"));
        }
        auto reader = FileReader::open(canonical);
        if (!reader)
            return std::unexpected(reader.error());
        if ((*reader)->size() > remaining_bytes)
            return std::unexpected(invalid("Explicit disk members exceed the 8 GiB inspection limit"));
        remaining_bytes -= (*reader)->size();
        auto digest = package_internal::sha256_reader(**reader, cancellation);
        if (!digest)
            return std::unexpected(digest.error());
        // Bind the directory geometry to the same fingerprint interval as the sampler records.
        SourceMember member{
            "source-" + std::to_string(members.size() + 1U), text::path_to_utf8(canonical.filename()), *reader, {}};
        auto is_sfs = sfs.recognizes(**reader);
        if (!is_sfs)
            return std::unexpected(is_sfs.error());
        if (*is_sfs) {
            if (auto added = sfs.admit(member, canonical, package_internal::hex_digest(*digest), cancellation); !added)
                return std::unexpected(added.error());
        } else {
            auto disk = FatImage::open(*reader, member.label, cancellation);
            if (!disk)
                return std::unexpected(disk.error());
            auto root = filesystems::browse_fat(std::make_shared<FatImage>(std::move(*disk)), 0U, cancellation);
            if (!root)
                return std::unexpected(root.error());
            member.roots.push_back(std::move(*root));
        }
        members.push_back(std::move(member));
        files.push_back({canonical, *digest, (*reader)->size()});
    }
    auto set = SourceSet::create(std::move(members));
    if (!set)
        return std::unexpected(set.error());
    FormatRegistry registry;
    if (auto added = registry.add(std::move(sfs).provider()); !added)
        return std::unexpected(added.error());
    if (auto added = registry.add(typhoon::provider()); !added)
        return std::unexpected(added.error());
    if (auto added = registry.add(tx16w::provider()); !added)
        return std::unexpected(added.error());
    auto result = inspect(*set, registry, cancellation);
    if (!result)
        return result;
    // The handle exposes only the retained catalog. Verify the path again before publishing it.
    for (const auto &file : files) {
        auto reader = FileReader::open(file.path);
        if (!reader)
            return std::unexpected(reader.error());
        if ((*reader)->size() != file.size)
            return std::unexpected(make_error(ErrorCode::transaction_stale, ErrorCategory::io,
                                              "Disk size changed during native inspection"));
        auto digest = package_internal::sha256_reader(**reader, cancellation);
        if (!digest)
            return std::unexpected(digest.error());
        if (*digest != file.digest)
            return std::unexpected(
                make_error(ErrorCode::transaction_stale, ErrorCategory::io, "Disk changed during native inspection"));
    }
    return result;
}
} // namespace axk::inspection
