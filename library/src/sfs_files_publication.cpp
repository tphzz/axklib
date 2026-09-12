#include "sfs_files_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "axklib/file_publication.hpp"
#include "axklib/package_archive.hpp"

namespace axk {
namespace {
Result<void> copy_range(const RandomAccessReader &source, detail::TemporaryPublication &destination,
                        std::uint64_t source_offset, std::uint64_t destination_offset, std::uint64_t size,
                        std::span<std::byte> buffer, const CancellationToken &cancellation, ProgressSink *progress) {
    if (source_offset > source.size() || size > source.size() - source_offset)
        return std::unexpected{sfs_files::error("filesystem input range exceeds its source")};
    for (std::uint64_t copied = 0; copied < size;) {
        if (auto checked = cancellation.check(); !checked)
            return checked;
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - copied));
        const auto chunk = buffer.first(count);
        if (auto read = source.read_exact_at(source_offset + copied, chunk); !read)
            return read;
        if (auto written = destination.write_at(destination_offset + copied, chunk); !written)
            return written;
        copied += count;
        if (progress)
            progress->report({ProgressPhase::writing, copied, size, "Writing filesystem data", std::nullopt});
    }
    return {};
}

Result<void> validate_written(const std::filesystem::path &path, PartitionIndex partition,
                              const CancellationToken &cancellation) {
    OpenOptions options;
    options.cancellation = cancellation;
    auto reopened = open_image(path, options);
    if (!reopened)
        return std::unexpected{reopened.error()};
    return sfs_files::validate(*reopened, partition);
}
} // namespace

Result<PublicationOutcome> write_sfs_file_edits(const std::filesystem::path &source_path,
                                                const std::filesystem::path &destination, PartitionIndex partition,
                                                std::span<const FilesystemEdit> edits,
                                                const CancellationToken &cancellation, ProgressSink *progress) {
    if (auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    auto source = FileReader::open(source_path);
    if (!source)
        return std::unexpected{source.error()};
    std::map<std::shared_ptr<const RandomAccessReader>, package_internal::Sha256Digest> input_digests;
    for (const auto &edit : edits) {
        const auto *put = std::get_if<PutFilesystemFile>(&edit);
        if (!put || !put->contents || input_digests.contains(put->contents))
            continue;
        if (put->contents->size() > std::numeric_limits<std::uint32_t>::max())
            return std::unexpected{sfs_files::error("file input exceeds the SFS size field")};
        auto digest = package_internal::sha256_reader(*put->contents, cancellation);
        if (!digest)
            return std::unexpected{digest.error()};
        input_digests.emplace(put->contents, *digest);
    }
    const auto snapshot = package_internal::sha256_reader(**source, cancellation);
    if (!snapshot)
        return std::unexpected(snapshot.error());
    auto prepared = detail::prepare_sfs_file_edits(*source, partition, edits, cancellation);
    if (!prepared)
        return std::unexpected{prepared.error()};
    auto publication = detail::TemporaryPublication::create(destination);
    if (!publication)
        return std::unexpected{publication.error()};
    std::vector<std::byte> buffer(1024U * 1024U);
    if (auto copied = copy_range(**source, *publication, 0U, 0U, (*source)->size(), buffer, cancellation, progress);
        !copied)
        return std::unexpected{copied.error()};
    for (const auto &patch : prepared->patches) {
        if (auto copied = copy_range(*patch.source, *publication, patch.source_offset, patch.offset, patch.size, buffer,
                                     cancellation, progress);
            !copied)
            return std::unexpected{copied.error()};
    }
    if (auto flushed = publication->flush(); !flushed)
        return std::unexpected{flushed.error()};
    if (auto checked = validate_written(publication->path(), partition, cancellation); !checked)
        return std::unexpected{checked.error()};
    const auto unchanged = package_internal::sha256_reader(**source, cancellation);
    if (!unchanged)
        return std::unexpected{unchanged.error()};
    if (*unchanged != *snapshot)
        return std::unexpected{
            sfs_files::error("source image changed during filesystem editing", ErrorCode::transaction_stale)};
    for (const auto &[reader, expected] : input_digests) {
        const auto digest = package_internal::sha256_reader(*reader, cancellation);
        if (!digest)
            return std::unexpected{digest.error()};
        if (*digest != expected)
            return std::unexpected{
                sfs_files::error("file input changed during filesystem editing", ErrorCode::transaction_stale)};
    }
    if (auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    return publication->publish(detail::PublicationMode::create_only);
}
} // namespace axk
