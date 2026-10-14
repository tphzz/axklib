#include "formats/a_series/admission.hpp"

#include <algorithm>
#include <set>
#include <variant>

#include "axklib/formats/a_series/native_document.hpp"
#include "axklib/sfs.hpp"
#include "axklib/system_file.hpp"
#include "axklib/utf8.hpp"
#include "formats/a_series/directory.hpp"
#include "formats/a_series/floppy_set.hpp"
#include "formats/a_series/session_state.hpp"
#include "image_sessions_internal.hpp"

namespace axk::app::formats::a_series {
namespace {
bool contains_system_file(const MediaContainer &media) {
    const auto *image = std::get_if<Container>(&media.storage());
    if (!image)
        return false;
    for (const auto &partition : image->partitions()) {
        for (const auto kind : {SystemFileKind::a3000_system, SystemFileKind::a4000_a5000_system2}) {
            const auto id = locate_system_file_record(partition, kind);
            if (!id || !*id)
                continue;
            const auto record = std::ranges::find(partition.records, **id, &IndexRecord::sfs_id);
            const auto size = system_file_record_size(kind);
            if (record == partition.records.end() || record->data_size != size)
                continue;
            const auto bytes = image->read_record_data(partition.index, **id, size);
            if (bytes && decode_system_file(kind, *bytes))
                return true;
        }
    }
    return false;
}
} // namespace

Result<std::vector<NativeDocumentAdmission>> admit_documents(const Sandbox &, const AdmittedImageSources &sources,
                                                             const CancellationToken &cancellation,
                                                             ProgressSink *progress, diagnostic_sink *diagnostics) {
    std::vector<NativeDocumentAdmission> result;
    std::set<std::string> consumed;
    for (const auto &source : sources.members) {
        if (consumed.contains(source->id))
            continue;
        if (auto active = cancellation.check(); !active)
            return std::unexpected(core_error(active.error(), source->reference));
        NativeDocumentAdmission item;
        item.id = "document-a-series-" + source->id;
        item.primary = source;
        item.member_ids.push_back(source->id);
        item.snapshot = source->reader;
        std::optional<MediaContainer> media;
        if (source->reference.kind == ImageSourceKind::directory) {
            std::vector<std::shared_ptr<ImageSourceMember>> companions;
            for (const auto &candidate : sources.members)
                if (candidate != source && candidate->reference.kind == ImageSourceKind::directory &&
                    !consumed.contains(candidate->id))
                    companions.push_back(candidate);
            auto directory = open_directory_source(*source, companions, cancellation);
            if (!directory) {
                if (directory.error().code != "native_format_unrecognized")
                    return std::unexpected(directory.error());
                continue;
            }
            item.snapshot = directory->snapshot;
            item.floppy_set = directory->opened.summary;
            item.companion_sources = directory->opened.companion_sources;
            for (const auto &candidate : companions)
                if (std::ranges::find(item.companion_sources, candidate->reference) != item.companion_sources.end())
                    item.member_ids.push_back(candidate->id);
            media.emplace(std::move(directory->opened.media));
        } else if (source->media) {
            media.emplace(*source->media);
            if (source->media->kind() == MediaKind::fat12_floppy ||
                source->media->kind() == MediaKind::fat12_floppy_set) {
                std::vector<std::shared_ptr<ImageSourceMember>> companions;
                const auto *fat = std::get_if<FatImage>(&source->media->storage());
                if (fat && fat->disk_identity().trusted_for_disk_set) {
                    for (const auto &candidate : sources.members) {
                        if (candidate == source || !candidate->media || consumed.contains(candidate->id))
                            continue;
                        const auto *other = std::get_if<FatImage>(&candidate->media->storage());
                        if (other && other->disk_identity().trusted_for_disk_set &&
                            other->disk_identity().set_name == fat->disk_identity().set_name) {
                            item.companion_sources.push_back(candidate->reference);
                            item.member_ids.push_back(candidate->id);
                            companions.push_back(candidate);
                        }
                    }
                }
                auto floppy = open_floppy_source(*source, companions, cancellation);
                if (!floppy)
                    return std::unexpected(floppy.error());
                item.floppy_set = floppy->summary;
                media.emplace(std::move(floppy->media));
            }
        } else {
            continue;
        }
        auto decoded = axk::formats::a_series::Document::open(std::move(*media), cancellation, progress, diagnostics);
        if (!decoded)
            return std::unexpected(core_error(decoded.error(), source->reference));
        auto projected = SessionState::create(*decoded, source->reference, diagnostics);
        if (!projected)
            return std::unexpected(projected.error());
        if ((*projected)->objects.empty()) {
            if ((*projected)->media->kind() != MediaKind::sfs)
                continue;
            auto files = detail::build_image_filesystem(*(*projected)->media, source->reader,
                                                        (*projected)->filesystem_bindings());
            if (!files)
                return std::unexpected(files.error());
            if (!files->device_view && !contains_system_file(*(*projected)->media))
                continue;
        }
        item.media = (*projected)->media;
        item.format = media_kind_name(item.media->kind());
        axk::formats::a_series::NativeDocument::Locations locations;
        if (item.media->kind() == MediaKind::standalone_object) {
            const auto location = sources.native_sources->locate(source->id, "raw-file", "file");
            if (location)
                for (const auto &object : (*decoded)->state().inventory.objects)
                    locations[object.key].push_back(*location);
        }
        if (source->media && item.member_ids.size() == 1U) {
            auto bindings =
                detail::build_image_filesystem(*source->media, source->reader, (*projected)->filesystem_bindings());
            if (!bindings)
                return std::unexpected(bindings.error());
            for (const auto &entry : bindings->entries) {
                if (!entry.object_id)
                    continue;
                const auto object = (*projected)->snapshots_by_id.find(*entry.object_id);
                if (object == (*projected)->snapshots_by_id.end())
                    continue;
                auto location = sources.native_sources->locate(source->id, entry.root_id, entry.id);
                if (location)
                    locations[object->second->key].push_back(*location);
            }
        }
        for (const auto &descriptor : (*decoded)->state().inventory.objects) {
            if (descriptor.source_files.empty())
                continue;
            auto &bound = locations[descriptor.key];
            bound.clear();
            for (const auto &file : descriptor.source_files)
                for (const auto &member : sources.members) {
                    if (file.source_name != member->id &&
                        file.source_name != text::path_to_utf8(member->canonical_path))
                        continue;
                    for (const auto &root : member->roots)
                        for (const auto &entry : root.entries)
                            if (entry.path == file.path) {
                                auto location = sources.native_sources->locate(member->id, root.id, entry.id);
                                if (location)
                                    bound.push_back(*location);
                            }
                }
        }
        auto common = axk::formats::a_series::NativeDocument::create(*decoded, sources.native_sources, item.id,
                                                                     std::move(locations));
        if (!common)
            return std::unexpected(core_error(common.error(), source->reference));
        (*projected)->native_document = std::move(*common);
        item.native = std::move(*projected);
        consumed.insert(item.member_ids.begin(), item.member_ids.end());
        result.push_back(std::move(item));
    }
    return result;
}

} // namespace axk::app::formats::a_series
