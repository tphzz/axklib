#include "image_sessions_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

axk::app::Result<std::vector<std::byte>>
axk::app::ImageSessionManager::Implementation::read_object_range(const Session &session, std::string_view object_id,
                                                                 std::uint64_t offset, std::size_t size,
                                                                 const CancellationToken &cancellation) const {
    const auto snapshot = session.snapshots_by_id.find(std::string{object_id});
    const auto descriptor = session.descriptors_by_id.find(std::string{object_id});
    if (snapshot == session.snapshots_by_id.end() || descriptor == session.descriptors_by_id.end())
        return std::unexpected(session_error("object_not_found", "image object does not exist"));
    if (offset > descriptor->second.size || size > descriptor->second.size - offset)
        return std::unexpected(session_error("invalid_audio_range", "audio source range exceeds the object"));
    if (const auto *sfs = std::get_if<Container>(&session.media->storage())) {
        auto bytes =
            sfs->read_record_range(snapshot->second.partition, snapshot->second.sfs_id, offset, size, cancellation);
        if (!bytes)
            return std::unexpected(core_error(bytes.error(), session.source));
        return std::move(*bytes);
    }
    if (const auto *fat = std::get_if<FatImage>(&session.media->storage())) {
        const auto file = std::ranges::find(fat->files(), descriptor->second.logical_path, &FatFile::path);
        if (file == fat->files().end())
            return std::unexpected(session_error("object_not_found", "FAT12 object file does not exist"));
        auto bytes = fat->read_file_range(*file, offset, size, cancellation);
        if (!bytes)
            return std::unexpected(core_error(bytes.error(), session.source));
        return std::move(*bytes);
    }
    if (const auto *iso = std::get_if<IsoImage>(&session.media->storage())) {
        const auto file = std::ranges::find(iso->files(), descriptor->second.logical_path, &IsoFile::path);
        if (file == iso->files().end())
            return std::unexpected(session_error("object_not_found", "ISO object file does not exist"));
        auto bytes = iso->read_file_range(*file, offset, size, cancellation);
        if (!bytes)
            return std::unexpected(core_error(bytes.error(), session.source));
        return std::move(*bytes);
    }
    if (const auto *directory = std::get_if<AxkObjectDirectory>(&session.media->storage())) {
        const auto object = std::ranges::find(directory->stored_objects(), descriptor->second.key, &MediaObject::key);
        if (object == directory->stored_objects().end())
            return std::unexpected(session_error("object_not_found", "AXK object directory entry does not exist"));
        if (offset > object->raw_payload.size() || size > object->raw_payload.size() - offset)
            return std::unexpected(session_error("invalid_audio_range", "AXK object directory range is invalid"));
        return std::vector<std::byte>{object->raw_payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                      object->raw_payload.begin() + static_cast<std::ptrdiff_t>(offset + size)};
    }
    if (const auto *archive = std::get_if<A3kArchive>(&session.media->storage())) {
        const auto entry = std::ranges::find_if(archive->entries(), [&](const A3kArchiveEntry &candidate) {
            return descriptor->second.key == std::format("a3k:{}", candidate.ordinal) &&
                   descriptor->second.data_offset == candidate.offset && descriptor->second.size == candidate.size;
        });
        if (entry == archive->entries().end())
            return std::unexpected(session_error("object_not_found", "A3K archive entry does not exist"));
        auto bytes = archive->read_entry_range(*entry, offset, size, cancellation);
        if (!bytes)
            return std::unexpected(core_error(bytes.error(), session.source));
        return std::move(*bytes);
    }
    const auto &payload = snapshot->second.raw_payload;
    if (offset > payload.size() || size > payload.size() - offset)
        return std::unexpected(session_error("invalid_audio_range", "standalone object range is invalid"));
    return std::vector<std::byte>{payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                  payload.begin() + static_cast<std::ptrdiff_t>(offset + size)};
}
