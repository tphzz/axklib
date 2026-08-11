#include "axklib/media.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "axklib/catalog_internal.hpp"
#include "media_internal.hpp"

namespace axk {
namespace {

MediaObjectDescriptor describe_media_object(const MediaObject &object) {
    return {
        object.key,         object.logical_path, object.scope_key,   object.raw_group, object.raw_volume,
        object.group_label, object.volume_label, object.data_offset, object.size,
    };
}

MediaObjectDescriptor describe_catalog_object(const ObjectSnapshot &object, std::uint64_t stored_size) {
    const auto &placement = object.placement;
    return {
        object.key,
        placement ? std::format("{}/{}/{}", placement->volume_name, placement->category_name, placement->entry_name)
                  : object.key,
        object.scope_key,
        {},
        {},
        {placement ? placement->partition_name : std::string{}, LabelStatus::confirmed, "SFS partition directory"},
        {placement ? placement->volume_name : std::string{}, LabelStatus::confirmed, "SFS volume directory"},
        0U,
        stored_size,
    };
}

ObjectCatalog catalog_from_media_objects(std::vector<MediaObject> objects) {
    ObjectCatalog result;
    std::map<std::pair<std::string, std::string>, std::uint32_t> volume_ids;
    std::uint32_t next_volume = 1;
    std::uint32_t next_object = 1;
    for (auto &object : objects) {
        const auto scope = std::pair{object.raw_group, object.raw_volume};
        if (!volume_ids.contains(scope))
            volume_ids.emplace(scope, next_volume++);
        const auto id = next_object++;
        ObjectPlacement placement{PartitionIndex{0},
                                  object.group_label.value,
                                  SfsId{volume_ids.at(scope)},
                                  object.volume_label.value,
                                  detail::object_category(object.decoded.header.type),
                                  object.decoded.header.name,
                                  object.raw_group.empty() ? std::string{}
                                  : object.raw_volume.empty()
                                      ? object.raw_group
                                      : std::format("{}/{}", object.raw_group, object.raw_volume)};
        std::vector<ObjectPlacement> placement_candidates{placement};
        result.objects.push_back({object.key, PartitionIndex{0}, SfsId{id}, object.scope_key, std::move(object.decoded),
                                  placement, std::move(object.raw_payload), std::move(placement_candidates),
                                  PlacementResolution::exact});
        if (object.decode_issue) {
            result.issues.push_back(
                {"media_object_decode_failed", render_error(*object.decode_issue), PartitionIndex{0}, SfsId{id}});
        }
    }
    return result;
}

} // namespace

Result<ObjectCatalog> build_object_catalog(const MediaContainer &container, std::size_t maximum_object_bytes,
                                           const CancellationToken &cancellation) {
    auto inventory =
        build_media_inventory(container, MediaObjectReadMode::complete, maximum_object_bytes, cancellation);
    if (!inventory)
        return std::unexpected{inventory.error()};
    return std::move(inventory->catalog);
}

Result<MediaInventory> build_media_inventory(const MediaContainer &container, MediaObjectReadMode mode,
                                             std::size_t maximum_object_bytes, const CancellationToken &cancellation) {
    if (const auto *sfs = std::get_if<Container>(&container.storage())) {
        const bool retain_raw_payloads = mode == MediaObjectReadMode::complete;
        auto catalog = detail::build_object_catalog(*sfs, maximum_object_bytes, cancellation, retain_raw_payloads);
        if (!catalog)
            return std::unexpected{catalog.error()};
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint64_t> stored_sizes;
        for (const auto &partition : sfs->partitions()) {
            for (const auto &record : partition.records)
                stored_sizes.emplace(std::pair{partition.index.value, record.sfs_id.value}, record.data_size);
        }
        std::vector<MediaObjectDescriptor> objects;
        objects.reserve(catalog->objects.size());
        for (const auto &object : catalog->objects) {
            const auto stored = stored_sizes.find({object.partition.value, object.sfs_id.value});
            const auto stored_size = stored == stored_sizes.end() ? object.raw_payload.size() : stored->second;
            objects.push_back(describe_catalog_object(object, stored_size));
        }
        return MediaInventory{std::move(objects), std::move(*catalog), retain_raw_payloads};
    }

    auto loaded = container.objects(mode, maximum_object_bytes, cancellation);
    if (!loaded)
        return std::unexpected{loaded.error()};
    std::vector<MediaObjectDescriptor> objects;
    objects.reserve(loaded->size());
    for (const auto &object : *loaded)
        objects.push_back(describe_media_object(object));
    const bool raw_payloads_complete =
        mode == MediaObjectReadMode::complete || container.kind() == MediaKind::standalone_object;
    return MediaInventory{std::move(objects), catalog_from_media_objects(std::move(*loaded)), raw_payloads_complete};
}

Result<MediaObject> load_media_object(const MediaContainer &container, const MediaObjectDescriptor &descriptor,
                                      std::size_t maximum_object_bytes, const CancellationToken &cancellation) {
    if (descriptor.size > maximum_object_bytes) {
        return std::unexpected{make_error(ErrorCode::io_unsupported_size, ErrorCategory::io,
                                          "media object exceeds the configured size limit")};
    }
    if (const auto *sfs = std::get_if<Container>(&container.storage())) {
        auto catalog = detail::build_object_catalog(*sfs, maximum_object_bytes, cancellation, false);
        if (!catalog)
            return std::unexpected{catalog.error()};
        const auto found = std::ranges::find(catalog->objects, descriptor.key, &ObjectSnapshot::key);
        if (found == catalog->objects.end())
            return std::unexpected{make_error(ErrorCode::object_missing, ErrorCategory::object,
                                              "media object is not present in the image")};
        auto bytes = sfs->read_record_data(found->partition, found->sfs_id, maximum_object_bytes, cancellation);
        if (!bytes)
            return std::unexpected{bytes.error()};
        auto decoded = detail::decode_media_object(*bytes, bytes->size());
        if (!decoded)
            return std::unexpected{decoded.error()};
        return MediaObject{
            descriptor.key,        descriptor.logical_path,    descriptor.scope_key,    descriptor.raw_group,
            descriptor.raw_volume, descriptor.group_label,     descriptor.volume_label, descriptor.data_offset,
            bytes->size(),         std::move(decoded->object), std::move(*bytes),       std::move(decoded->issue)};
    }

    if (const auto *set = std::get_if<FloppyDiskSet>(&container.storage())) {
        auto objects = set->objects(MediaObjectReadMode::complete, maximum_object_bytes, cancellation);
        if (!objects)
            return std::unexpected{objects.error()};
        const auto found = std::ranges::find(*objects, descriptor.key, &MediaObject::key);
        if (found == objects->end()) {
            return std::unexpected{make_error(ErrorCode::object_missing, ErrorCategory::object,
                                              "media object is not present in the floppy disk set")};
        }
        return *found;
    }

    if (const auto *archive = std::get_if<A3kArchive>(&container.storage())) {
        const auto found = std::ranges::find_if(archive->entries(), [&](const A3kArchiveEntry &entry) {
            return descriptor.key == std::format("a3k:{}", entry.ordinal) && descriptor.data_offset == entry.offset &&
                   descriptor.size == entry.size;
        });
        if (found == archive->entries().end()) {
            return std::unexpected{make_error(ErrorCode::object_missing, ErrorCategory::object,
                                              "media object is not present in the A3K archive")};
        }
        auto bytes = archive->read_entry(*found, cancellation);
        if (!bytes)
            return std::unexpected{bytes.error()};
        auto decoded = detail::decode_media_object(*bytes, found->size);
        if (!decoded)
            return std::unexpected{decoded.error()};
        return MediaObject{
            descriptor.key,        descriptor.logical_path,    descriptor.scope_key,    descriptor.raw_group,
            descriptor.raw_volume, descriptor.group_label,     descriptor.volume_label, descriptor.data_offset,
            bytes->size(),         std::move(decoded->object), std::move(*bytes),       std::move(decoded->issue)};
    }

    std::vector<std::byte> bytes;
    if (const auto *fat = std::get_if<FatImage>(&container.storage())) {
        const auto file = std::ranges::find(fat->files(), descriptor.logical_path, &FatFile::path);
        if (file == fat->files().end())
            return std::unexpected{make_error(ErrorCode::object_missing, ErrorCategory::object,
                                              "media object is not present in the FAT12 image")};
        auto loaded = fat->read_file(*file, cancellation);
        if (!loaded)
            return std::unexpected{loaded.error()};
        bytes = std::move(*loaded);
    } else if (const auto *iso = std::get_if<IsoImage>(&container.storage())) {
        const auto file = std::ranges::find(iso->files(), descriptor.logical_path, &IsoFile::path);
        if (file == iso->files().end() || file->is_directory)
            return std::unexpected{make_error(ErrorCode::object_missing, ErrorCategory::object,
                                              "media object is not present in the ISO9660 image")};
        auto loaded = iso->read_file(*file, cancellation);
        if (!loaded)
            return std::unexpected{loaded.error()};
        bytes = std::move(*loaded);
    } else if (const auto *standalone = std::get_if<StandaloneObject>(&container.storage())) {
        if (standalone->object().key != descriptor.key)
            return std::unexpected{make_error(ErrorCode::object_missing, ErrorCategory::object,
                                              "standalone media object does not match the descriptor")};
        return standalone->object();
    } else if (const auto *directory = std::get_if<AxkObjectDirectory>(&container.storage())) {
        const auto found = std::ranges::find(directory->stored_objects(), descriptor.key, &MediaObject::key);
        if (found == directory->stored_objects().end()) {
            return std::unexpected{make_error(ErrorCode::object_missing, ErrorCategory::object,
                                              "media object is not present in the AXK object directory")};
        }
        return *found;
    }
    auto decoded = detail::decode_media_object(bytes, bytes.size());
    if (!decoded)
        return std::unexpected{decoded.error()};
    return MediaObject{
        descriptor.key,        descriptor.logical_path,    descriptor.scope_key,    descriptor.raw_group,
        descriptor.raw_volume, descriptor.group_label,     descriptor.volume_label, descriptor.data_offset,
        bytes.size(),          std::move(decoded->object), std::move(bytes),        std::move(decoded->issue)};
}

std::string sanitize_path_component(std::string_view value, std::string_view fallback) {
    auto text = std::string{value};
    const auto first = text.find_first_not_of(" \t\r\n");
    const auto last = text.find_last_not_of(" \t\r\n");
    text = first == std::string::npos ? std::string{fallback} : text.substr(first, last - first + 1U);
    std::size_t duplicate_count{};
    while (!text.empty() && text.back() == '*') {
        ++duplicate_count;
        text.pop_back();
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
        text.pop_back();

    std::string result;
    result.reserve(text.size());
    bool previous_space{};
    bool previous_underscore{};
    for (const auto ch : text) {
        if (ch == '<' || ch == '>') {
            result += ch == '<' ? "_lt_" : "_gt_";
            previous_space = false;
            previous_underscore = true;
            continue;
        }
        const auto byte = static_cast<unsigned char>(ch);
        const auto invalid = byte < 0x20U || byte == 0x7fU || std::string_view{"/:*?\"|"}.contains(ch);
        if (invalid || ch == '_') {
            if (!previous_underscore)
                result.push_back('_');
            previous_space = false;
            previous_underscore = true;
        } else if (std::isspace(byte) != 0) {
            if (!result.empty() && !previous_space)
                result.push_back(' ');
            previous_space = true;
            previous_underscore = false;
        } else {
            result.push_back(ch);
            previous_space = false;
            previous_underscore = false;
        }
    }
    while (!result.empty() && (result.back() == ' ' || result.back() == '.' || result.back() == '_'))
        result.pop_back();
    while (!result.empty() && (result.front() == ' ' || result.front() == '.' || result.front() == '_'))
        result.erase(result.begin());
    if (result.empty() || result == "." || result == "..")
        result = std::string{fallback};
    if (duplicate_count != 0U)
        result += std::format(" ({})", duplicate_count + 1U);
    return result;
}

StructuredObjectPath structured_object_path(const MediaObject &object) {
    auto volume = sanitize_path_component(object.volume_label.value, "volume");
    auto category = sanitize_path_component(detail::object_category(object.decoded.header.type), "objects");
    auto name = sanitize_path_component(object.decoded.header.name, "unnamed");
    std::filesystem::path path;
    if (!object.group_label.value.empty())
        path /= sanitize_path_component(object.group_label.value, "objects");
    path /= volume;
    path /= category;
    path /= name;
    return {std::move(path), object.group_label, object.volume_label};
}

std::vector<StructuredObjectPath> structured_object_paths(std::span<const MediaObject> objects) {
    std::map<std::pair<std::string, std::string>, std::set<std::string>> raw_volumes;
    for (const auto &object : objects) {
        raw_volumes[{detail::upper_ascii(object.group_label.value), detail::upper_ascii(object.volume_label.value)}]
            .insert(detail::upper_ascii(object.raw_volume));
    }
    std::vector<StructuredObjectPath> result;
    result.reserve(objects.size());
    for (const auto &object : objects) {
        auto path = structured_object_path(object);
        const auto &volumes = raw_volumes.at(
            {detail::upper_ascii(object.group_label.value), detail::upper_ascii(object.volume_label.value)});
        if (volumes.size() > 1U && !object.raw_volume.empty()) {
            auto volume =
                sanitize_path_component(std::format("{} ({})", object.volume_label.value, object.raw_volume), "volume");
            auto category = sanitize_path_component(detail::object_category(object.decoded.header.type), "objects");
            auto name = sanitize_path_component(object.decoded.header.name, "unnamed");
            path.relative_path.clear();
            if (!object.group_label.value.empty())
                path.relative_path /= sanitize_path_component(object.group_label.value, "objects");
            path.relative_path /= volume;
            path.relative_path /= category;
            path.relative_path /= name;
        }
        result.push_back(std::move(path));
    }
    return result;
}
} // namespace axk
