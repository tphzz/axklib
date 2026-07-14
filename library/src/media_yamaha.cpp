#include "axklib/media.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <tuple>

#include "axklib/bytes.hpp"
#include "axklib/utf8.hpp"
#include "media_internal.hpp"

namespace axk {
namespace {

std::string clean_label(std::span<const std::byte> bytes) {
    auto result = detail::clean_ascii(bytes);
    const auto first = result.find_first_not_of(' ');
    return first == std::string::npos ? std::string{} : result.substr(first);
}

struct MediaDecode {
    DecodedObject object;
    std::optional<Error> issue;
};

Result<MediaDecode> decode_media_object(std::span<const std::byte> bytes, std::uint64_t stored_size) {
    auto decoded = decode_object(bytes);
    if (decoded) {
        std::optional<Error> issue;
        if (std::holds_alternative<CurrentSmpl>(decoded->payload)) {
            const auto pcm_end = checked_add(decoded->header.header_size, decoded->header.payload_bytes_0x1c);
            if (!pcm_end || *pcm_end > stored_size) {
                issue = make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                   "SMPL stored PCM range exceeds the object payload");
            }
        }
        return MediaDecode{std::move(*decoded), std::move(issue)};
    }
    auto header = decode_object_header(bytes);
    if (!header)
        return std::unexpected{header.error()};
    std::vector<std::byte> raw{bytes.begin(), bytes.end()};
    return MediaDecode{DecodedObject{std::move(*header), ObjectFormat::unknown, GenericObject{std::move(raw)}},
                       decoded.error()};
}

std::vector<std::string> path_parts(std::string_view path) {
    std::vector<std::string> result;
    for (const auto part : std::views::split(path, '/'))
        result.emplace_back(part.begin(), part.end());
    return result;
}

bool f_directory(std::string_view value) {
    return value.size() == 4U && value[0] == 'F' &&
           std::ranges::all_of(value.substr(1), [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

std::string lookup_label(const std::vector<std::pair<std::string, std::string>> &labels, std::string_view key) {
    const auto found = std::ranges::find(labels, key, &std::pair<std::string, std::string>::first);
    return found == labels.end() ? std::string{} : found->second;
}

std::string object_category(ObjectType type) {
    switch (type) {
    case ObjectType::smpl:
        return "SMPL";
    case ObjectType::sbnk:
        return "SBNK";
    case ObjectType::sbac:
        return "SBAC";
    case ObjectType::prog:
        return "PROG";
    case ObjectType::sequ:
        return "SEQU";
    case ObjectType::prf3:
        return "PRF3";
    case ObjectType::unknown:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

std::string fallback_label(const MediaObject &object, std::span<const std::byte> bytes) {
    if (object.decoded.header.type == ObjectType::prog && bytes.size() >= 0x88U) {
        const auto display = detail::clean_ascii(bytes.subspan(0x78, 16));
        if (!display.empty() && display != std::format("Pgm {}", object.decoded.header.name))
            return display;
        return {};
    }
    switch (object.decoded.header.type) {
    case ObjectType::sbac:
    case ObjectType::sbnk:
    case ObjectType::smpl:
    case ObjectType::sequ:
        return object.decoded.header.name;
    default:
        return {};
    }
}

void add_group_catalog_issue(detail::IsoMenuLabels &result, std::string code, std::string_view group,
                             std::string message, std::string recommended_next_check) {
    result.validation_issues.push_back({
        std::move(code),
        std::move(message),
        std::format("CD-ROM group menu (raw group '{}')", group),
        "hardware-confirmed Yamaha group 0000 catalog contract",
        std::move(recommended_next_check),
    });
}

bool yamaha_object_category(std::string_view value) {
    return value == "SMPL" || value == "SBNK" || value == "SBAC" || value == "PROG" || value == "SEQU" ||
           value == "PRF3";
}

void add_category_catalog_issue(detail::IsoMenuLabels &result, std::string code, std::string_view group,
                                std::string_view volume, std::string_view category, std::string message,
                                std::string recommended_next_check) {
    result.validation_issues.push_back({
        std::move(code),
        std::move(message),
        std::format("CD-ROM group '{}', volume '{}', {} objects", group, volume, category),
        "Yamaha category 0000 catalog and object placement contract",
        std::move(recommended_next_check),
    });
}

struct CategoryCatalogRecord {
    std::string label;
    std::string target;
};

std::vector<CategoryCatalogRecord> category_catalog_records(std::span<const std::byte> bytes, std::size_t stride,
                                                            std::size_t name_width, std::size_t target_offset) {
    std::vector<CategoryCatalogRecord> result;
    for (std::size_t offset = 0; offset + stride <= bytes.size(); offset += stride) {
        const auto record = bytes.subspan(offset, stride);
        if (std::ranges::all_of(record, [](std::byte value) { return value == std::byte{}; }))
            continue;
        result.push_back(
            {clean_label(record.subspan(1, name_width)), detail::clean_ascii(record.subspan(target_offset, 11))});
    }
    return result;
}

std::vector<CategoryCatalogRecord> shifted_category_catalog_records(std::span<const std::byte> bytes) {
    std::vector<CategoryCatalogRecord> result;
    if (bytes.size() < 28U)
        return result;
    result.push_back({clean_label(bytes.first(13U)), detail::clean_ascii(bytes.subspan(14U, 11U))});
    for (std::size_t offset = 28U; offset + 32U <= bytes.size(); offset += 32U) {
        const auto record = bytes.subspan(offset, 32U);
        if (std::ranges::all_of(record, [](std::byte value) { return value == std::byte{}; }))
            continue;
        result.push_back({clean_label(record.subspan(1U, 16U)), detail::clean_ascii(record.subspan(18U, 11U))});
    }
    return result;
}

std::size_t category_catalog_target_count(std::span<const CategoryCatalogRecord> records) {
    return static_cast<std::size_t>(std::ranges::count_if(
        records, [](const auto &record) { return f_directory(record.target) && record.target != "F000"; }));
}

} // namespace

namespace detail {

Result<IsoMenuLabels> read_yamaha_iso_menu_labels(const IsoImage &image, const CancellationToken &cancellation) {
    IsoMenuLabels result;
    std::map<std::string, std::set<std::string>> volumes;
    for (const auto &file : image.files()) {
        const auto parts = path_parts(file.path);
        if (file.is_directory && parts.size() == 2U && f_directory(parts[1]))
            volumes[parts[0]].insert(parts[1]);
    }
    for (const auto &[group, group_volumes] : volumes) {
        const auto table = std::ranges::find(image.files(), std::format("{}/0000", group), &IsoFile::path);
        if (table == image.files().end() || table->is_directory) {
            add_group_catalog_issue(result, "ISO_YAMAHA_GROUP_CATALOG_MISSING", group,
                                    std::format("CD-ROM group menu '{}' has no readable "
                                                "group-level 0000 catalog.",
                                                group),
                                    "Restore the group-level 0000 file before using this image on "
                                    "a Yamaha sampler.");
            continue;
        }
        auto bytes = image.read_file(*table, cancellation);
        if (!bytes)
            return std::unexpected{bytes.error()};
        if (bytes->empty() || bytes->size() % 32U != 0U) {
            add_group_catalog_issue(result, "ISO_YAMAHA_GROUP_CATALOG_SIZE_INVALID", group,
                                    std::format("CD-ROM group menu '{}' has a {}-byte 0000 "
                                                "catalog; Yamaha menu rows must be "
                                                "complete 32-byte records.",
                                                group, bytes->size()),
                                    "Rebuild the group catalog as complete 32-byte rows while "
                                    "preserving readable object "
                                    "files.");
        }
        bool disk_name_seen{};
        for (std::size_t offset = 0; offset + 32U <= bytes->size(); offset += 32U) {
            const auto record = std::span{*bytes}.subspan(offset, 32);
            const auto label = clean_label(record.subspan(1, 16));
            const auto id = clean_ascii(record.subspan(18, 11));
            if (label == "_DSKNAME") {
                disk_name_seen = true;
                const auto expected_id = std::format("F{:03}", group_volumes.size() + 1U);
                if (offset + 32U != bytes->size()) {
                    add_group_catalog_issue(result, "ISO_YAMAHA_DSKNAME_ROW_NOT_FINAL", group,
                                            std::format("CD-ROM group menu '{}' has a _DSKNAME row "
                                                        "that is not the final 0000 "
                                                        "catalog record.",
                                                        group),
                                            "Move the _DSKNAME row after every volume row before "
                                            "using this image on a Yamaha "
                                            "sampler.");
                    continue;
                }
                if (id != expected_id) {
                    add_group_catalog_issue(result, "ISO_YAMAHA_DSKNAME_TARGET_INVALID", group,
                                            std::format("CD-ROM group menu '{}' references group-label "
                                                        "file '{}' from _DSKNAME; "
                                                        "the expected file after {} volume(s) is '{}'.",
                                                        group, id, group_volumes.size(), expected_id),
                                            "Point the final _DSKNAME row to the next Fnnn file "
                                            "after the volume directories.");
                    continue;
                }
                const auto label_file =
                    std::ranges::find(image.files(), std::format("{}/{}", group, id), &IsoFile::path);
                if (label_file == image.files().end() || label_file->is_directory) {
                    add_group_catalog_issue(result, "ISO_YAMAHA_DSKNAME_LABEL_FILE_MISSING", group,
                                            std::format("CD-ROM group menu '{}' references missing "
                                                        "group-label file '{}'.",
                                                        group, id),
                                            "Restore the 16-byte group-label file referenced by "
                                            "the final _DSKNAME row.");
                    continue;
                }
                if (label_file->size != 16U) {
                    add_group_catalog_issue(result, "ISO_YAMAHA_DSKNAME_LABEL_SIZE_INVALID", group,
                                            std::format("CD-ROM group menu '{}' uses a {}-byte group-label "
                                                        "file '{}'; the Yamaha "
                                                        "catalog contract requires exactly 16 bytes.",
                                                        group, label_file->size, id),
                                            "Rewrite the referenced group label as one fixed-width "
                                            "16-byte file.");
                    continue;
                }
                auto label_bytes = image.read_file(*label_file, cancellation);
                if (!label_bytes)
                    return std::unexpected{label_bytes.error()};
                const auto group_label = clean_label(*label_bytes);
                if (group_label.empty()) {
                    add_group_catalog_issue(result, "ISO_YAMAHA_DSKNAME_LABEL_EMPTY", group,
                                            std::format("CD-ROM group menu '{}' has an empty "
                                                        "group-label file '{}'.",
                                                        group, id),
                                            "Write a non-empty sampler-visible group name into the "
                                            "16-byte label file.");
                    continue;
                }
                result.groups.emplace_back(group, group_label);
                continue;
            }
            if (!id.empty() && !label.empty() && group_volumes.contains(id))
                result.volumes.emplace_back(std::format("{}/{}", group, id), label);
        }
        if (!disk_name_seen) {
            add_group_catalog_issue(result, "ISO_YAMAHA_DSKNAME_ROW_MISSING", group,
                                    std::format("CD-ROM group menu '{}' does not end with the "
                                                "required _DSKNAME row.",
                                                group),
                                    "Append a NUL-padded _DSKNAME row that references the next "
                                    "Fnnn group-label file.");
        }
    }

    for (const auto &directory : image.files()) {
        const auto parts = path_parts(directory.path);
        if (!directory.is_directory || parts.size() != 3U || !f_directory(parts[1]) ||
            !yamaha_object_category(parts[2])) {
            continue;
        }
        const auto &group = parts[0];
        const auto &volume = parts[1];
        const auto &category = parts[2];
        std::set<std::string> object_files;
        for (const auto &file : image.files()) {
            const auto child_parts = path_parts(file.path);
            if (!file.is_directory && child_parts.size() == 4U && child_parts[0] == group && child_parts[1] == volume &&
                child_parts[2] == category && f_directory(child_parts[3]) && child_parts[3] != "F000") {
                object_files.insert(child_parts[3]);
            }
        }
        if (object_files.empty())
            continue;

        const auto catalog_path = std::format("{}/0000", directory.path);
        const auto catalog = std::ranges::find(image.files(), catalog_path, &IsoFile::path);
        if (catalog == image.files().end() || catalog->is_directory) {
            add_category_catalog_issue(result, "ISO_YAMAHA_CATEGORY_CATALOG_MISSING", group, volume, category,
                                       std::format("{} objects in CD-ROM volume '{}' have no readable "
                                                   "0000 catalog.",
                                                   category, volume),
                                       "Restore the category 0000 catalog while preserving the "
                                       "readable object files.");
            continue;
        }
        auto catalog_bytes = image.read_file(*catalog, cancellation);
        if (!catalog_bytes) {
            add_category_catalog_issue(result, "ISO_YAMAHA_CATEGORY_CATALOG_UNREADABLE", group, volume, category,
                                       std::format("{} objects in CD-ROM volume '{}' have a 0000 "
                                                   "catalog whose declared span "
                                                   "cannot be read: {}",
                                                   category, volume, catalog_bytes.error().message),
                                       "Repair the catalog extent while preserving independently "
                                       "readable object files.");
            continue;
        }
        const auto records32 = category_catalog_records(*catalog_bytes, 32U, 16U, 18U);
        const auto shifted_records = shifted_category_catalog_records(*catalog_bytes);
        const auto score32 = category_catalog_target_count(records32);
        const auto shifted_score = category_catalog_target_count(shifted_records);
        const auto &records = shifted_score > score32 ? shifted_records : records32;
        if (std::max(shifted_score, score32) == 0U) {
            add_category_catalog_issue(result, "ISO_YAMAHA_CATEGORY_CATALOG_ROWS_INVALID", group, volume, category,
                                       std::format("{} objects in CD-ROM volume '{}' have a 0000 "
                                                   "catalog with no recognizable "
                                                   "standard or shifted 32-byte Fnnn target rows.",
                                                   category, volume),
                                       "Restore a Yamaha category catalog matching the object-file "
                                       "targets.");
        }

        std::set<std::string> catalog_targets;
        for (const auto &record : records) {
            const auto &label = record.label;
            const auto &id = record.target;
            if (!f_directory(id) || id == "F000")
                continue;
            if (!catalog_targets.insert(id).second) {
                add_category_catalog_issue(result, "ISO_YAMAHA_CATEGORY_CATALOG_TARGET_DUPLICATE", group, volume,
                                           category,
                                           std::format("{} catalog in CD-ROM volume '{}' references "
                                                       "object file '{}' more than "
                                                       "once.",
                                                       category, volume, id),
                                           "Give each catalog row one distinct object file target.");
                continue;
            }
            const auto target_path = std::format("{}/{}", directory.path, id);
            const auto target = std::ranges::find(image.files(), target_path, &IsoFile::path);
            if (target == image.files().end() || target->is_directory) {
                add_category_catalog_issue(result, "ISO_YAMAHA_CATEGORY_OBJECT_MISSING", group, volume, category,
                                           std::format("{} catalog entry '{}' in CD-ROM volume '{}' "
                                                       "references missing object "
                                                       "file '{}'.",
                                                       category, label, volume, id),
                                           "Restore the referenced object file or remove the stale "
                                           "catalog row.");
            }
        }

        for (const auto &filename : object_files) {
            if (catalog_targets.contains(filename))
                continue;
            add_category_catalog_issue(result, "ISO_YAMAHA_CATEGORY_OBJECT_UNCATALOGED", group, volume, category,
                                       std::format("{} object file '{}' in CD-ROM volume '{}' has no "
                                                   "0000 catalog row and will "
                                                   "not be menu-addressable by name.",
                                                   category, filename, volume),
                                       "Add a catalog row for the readable object file before using "
                                       "this image on a Yamaha "
                                       "sampler.");
        }
    }
    return result;
}

} // namespace detail

Result<std::vector<MediaObject>> FatImage::objects(std::size_t maximum_object_bytes,
                                                   const CancellationToken &cancellation) const {
    return objects(MediaObjectReadMode::complete, maximum_object_bytes, cancellation);
}

Result<std::vector<MediaObject>> FatImage::objects(MediaObjectReadMode mode, std::size_t maximum_object_bytes,
                                                   const CancellationToken &cancellation) const {
    constexpr std::size_t metadata_prefix_size = 0xacU;
    std::vector<MediaObject> result;
    for (const auto &file : files_) {
        if (file.size > maximum_object_bytes)
            continue;
        auto bytes = mode == MediaObjectReadMode::decoded_metadata
                         ? read_file_prefix(file, metadata_prefix_size, cancellation)
                         : read_file(file, cancellation);
        if (!bytes)
            return std::unexpected{bytes.error()};
        if (!detail::object_prefix(*bytes))
            continue;
        const bool sample =
            bytes->size() >= 0x10U && detail::clean_ascii(std::span{*bytes}.subspan(0x0cU, 4U)) == "SMPL";
        if (mode == MediaObjectReadMode::decoded_metadata && !sample) {
            bytes = read_file(file, cancellation);
            if (!bytes)
                return std::unexpected{bytes.error()};
        }
        auto decoded = decode_media_object(*bytes, file.size);
        if (!decoded)
            return std::unexpected{decoded.error()};
        auto raw_payload =
            mode == MediaObjectReadMode::decoded_metadata && sample ? std::vector<std::byte>{} : std::move(*bytes);
        result.push_back({std::format("fat:{}", file.path),
                          file.path,
                          "fat-root",
                          {},
                          {},
                          {"", LabelStatus::raw_identifier, "FAT root has no group label"},
                          {"FAT root", LabelStatus::confirmed, "FAT12 root directory"},
                          file.first_data_offset,
                          file.size,
                          std::move(decoded->object),
                          std::move(raw_payload),
                          std::move(decoded->issue)});
    }
    return result;
}

Result<std::vector<MediaObject>> IsoImage::objects(std::size_t maximum_object_bytes,
                                                   const CancellationToken &cancellation) const {
    return objects(MediaObjectReadMode::complete, maximum_object_bytes, cancellation);
}

Result<std::vector<MediaObject>> IsoImage::objects(MediaObjectReadMode mode, std::size_t maximum_object_bytes,
                                                   const CancellationToken &cancellation) const {
    constexpr std::size_t metadata_prefix_size = 0xacU;
    struct PendingObject {
        MediaObject object;
        std::vector<std::byte> bytes;
    };
    std::vector<PendingObject> pending;
    for (const auto &file : files_) {
        if (file.is_directory || file.size > maximum_object_bytes)
            continue;
        const auto file_offset = static_cast<std::uint64_t>(file.extent_sector) * detail::iso_sector_size;
        if (file_offset > reader_->size() || file.size > reader_->size() - file_offset)
            continue;
        auto bytes = mode == MediaObjectReadMode::decoded_metadata
                         ? read_file_prefix(file, metadata_prefix_size, cancellation)
                         : read_file(file, cancellation);
        if (!bytes)
            return std::unexpected{bytes.error()};
        if (!detail::object_prefix(*bytes))
            continue;
        const bool sample =
            bytes->size() >= 0x10U && detail::clean_ascii(std::span{*bytes}.subspan(0x0cU, 4U)) == "SMPL";
        if (mode == MediaObjectReadMode::decoded_metadata && !sample) {
            bytes = read_file(file, cancellation);
            if (!bytes)
                return std::unexpected{bytes.error()};
        }
        auto decoded = decode_media_object(*bytes, file.size);
        if (!decoded)
            return std::unexpected{decoded.error()};
        const auto parts = path_parts(file.path);
        const auto group = parts.empty() ? std::string{} : parts[0];
        const auto volume = parts.size() < 2U ? std::string{} : parts[1];
        auto group_label = lookup_label(group_labels_, group);
        auto volume_label = lookup_label(volume_labels_, std::format("{}/{}", group, volume));
        pending.push_back({{std::format("iso9660:{}", file.path),
                            file.path,
                            std::format("iso:{}:iso9660", volume_id_),
                            group,
                            volume,
                            {group_label.empty() ? group : group_label,
                             group_label.empty() ? LabelStatus::raw_identifier : LabelStatus::confirmed,
                             group_label.empty() ? "ISO9660 raw group identifier" : "Yamaha CD-ROM menu label"},
                            {volume_label.empty() ? volume : volume_label,
                             volume_label.empty() ? LabelStatus::raw_identifier : LabelStatus::confirmed,
                             volume_label.empty() ? "ISO9660 raw volume identifier" : "Yamaha CD-ROM menu label"},
                            static_cast<std::uint64_t>(file.extent_sector) * detail::iso_sector_size,
                            file.size,
                            std::move(decoded->object),
                            {},
                            std::move(decoded->issue)},
                           std::move(*bytes)});
    }
    const std::map<ObjectType, int> priority{{ObjectType::prog, 0},
                                             {ObjectType::sbac, 1},
                                             {ObjectType::sbnk, 2},
                                             {ObjectType::smpl, 3},
                                             {ObjectType::sequ, 4}};
    std::map<std::pair<std::string, std::string>, std::vector<PendingObject *>> scopes;
    for (auto &item : pending) {
        if (item.object.volume_label.status != LabelStatus::confirmed)
            scopes[{item.object.raw_group, item.object.raw_volume}].push_back(&item);
    }
    std::map<std::string, std::set<std::string>> used;
    for (auto &[scope, items] : scopes) {
        std::ranges::sort(items, [&](const PendingObject *left, const PendingObject *right) {
            return std::tuple{priority.contains(left->object.decoded.header.type)
                                  ? priority.at(left->object.decoded.header.type)
                                  : 99,
                              left->object.decoded.header.name,
                              left->object.key} < std::tuple{priority.contains(right->object.decoded.header.type)
                                                                 ? priority.at(right->object.decoded.header.type)
                                                                 : 99,
                                                             right->object.decoded.header.name, right->object.key};
        });
        std::string derived;
        for (const auto *item : items) {
            derived = fallback_label(item->object, item->bytes);
            if (!derived.empty())
                break;
        }
        if (derived.empty())
            continue;
        auto display = derived;
        if (!used[scope.first].insert(detail::upper_ascii(display)).second)
            display = std::format("{} ({})", display, scope.second);
        for (auto *item : items) {
            item->object.volume_label = {display, LabelStatus::navigation_aid,
                                         "ISO directory path plus content-derived volume label "
                                         "fallback"};
        }
    }
    std::vector<MediaObject> result;
    result.reserve(pending.size());
    for (auto &item : pending) {
        if (mode == MediaObjectReadMode::complete || item.object.decoded.header.type != ObjectType::smpl)
            item.object.raw_payload = std::move(item.bytes);
        result.push_back(std::move(item.object));
    }
    std::ranges::sort(result, {}, &MediaObject::logical_path);
    return result;
}

Result<StandaloneObject> StandaloneObject::open(std::shared_ptr<const RandomAccessReader> reader,
                                                std::string source_name, std::size_t maximum_object_bytes) {
    if (!reader || reader->size() > maximum_object_bytes || reader->size() > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected{detail::media_error(ErrorCode::io_unsupported_size,
                                                   "standalone object exceeds the configured size limit", source_name)};
    }
    auto bytes = detail::read_bytes(*reader, 0, static_cast<std::size_t>(reader->size()), {});
    if (!bytes)
        return std::unexpected{bytes.error()};
    if (!detail::object_prefix(*bytes)) {
        return std::unexpected{detail::media_error(ErrorCode::container_unrecognized,
                                                   "file is not a standalone Yamaha object", source_name)};
    }
    auto decoded = decode_media_object(*bytes, reader->size());
    if (!decoded)
        return std::unexpected{decoded.error()};
    StandaloneObject result;
    result.object_ = {"standalone",
                      source_name,
                      "standalone",
                      {},
                      {},
                      {"", LabelStatus::raw_identifier, "standalone object has no group"},
                      {"Standalone object", LabelStatus::confirmed, "standalone object file"},
                      0,
                      reader->size(),
                      std::move(decoded->object),
                      std::move(*bytes),
                      std::move(decoded->issue)};
    return result;
}

Result<StandaloneObject> StandaloneObject::open(const std::filesystem::path &path, std::size_t maximum_object_bytes) {
    auto reader = FileReader::open(path);
    if (!reader)
        return std::unexpected{reader.error()};
    return open(std::move(*reader), text::path_to_utf8(path), maximum_object_bytes);
}

const MediaObject &StandaloneObject::object() const noexcept { return object_; }

namespace {

MediaObjectDescriptor describe_media_object(const MediaObject &object) {
    return {
        object.key,         object.logical_path, object.scope_key,   object.raw_group, object.raw_volume,
        object.group_label, object.volume_label, object.data_offset, object.size,
    };
}

MediaObjectDescriptor describe_catalog_object(const ObjectSnapshot &object) {
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
        object.raw_payload.size(),
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
                                  object_category(object.decoded.header.type),
                                  object.decoded.header.name,
                                  object.raw_group.empty() ? std::string{}
                                  : object.raw_volume.empty()
                                      ? object.raw_group
                                      : std::format("{}/{}", object.raw_group, object.raw_volume)};
        result.objects.push_back({object.key, PartitionIndex{0}, SfsId{id}, object.scope_key, std::move(object.decoded),
                                  std::move(placement), std::move(object.raw_payload)});
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
        auto catalog = build_object_catalog(*sfs, maximum_object_bytes, cancellation);
        if (!catalog)
            return std::unexpected{catalog.error()};
        std::vector<MediaObjectDescriptor> objects;
        objects.reserve(catalog->objects.size());
        for (const auto &object : catalog->objects)
            objects.push_back(describe_catalog_object(object));
        return MediaInventory{std::move(objects), std::move(*catalog), true};
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
    auto group = sanitize_path_component(object.group_label.value, "objects");
    auto volume = sanitize_path_component(object.volume_label.value, "volume");
    auto category = sanitize_path_component(object_category(object.decoded.header.type), "objects");
    auto name = sanitize_path_component(object.decoded.header.name, "unnamed");
    std::filesystem::path path{group};
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
            auto group = sanitize_path_component(object.group_label.value, "objects");
            auto volume =
                sanitize_path_component(std::format("{} ({})", object.volume_label.value, object.raw_volume), "volume");
            auto category = sanitize_path_component(object_category(object.decoded.header.type), "objects");
            auto name = sanitize_path_component(object.decoded.header.name, "unnamed");
            path.relative_path = std::filesystem::path{group} / volume / category / name;
        }
        result.push_back(std::move(path));
    }
    return result;
}

} // namespace axk
