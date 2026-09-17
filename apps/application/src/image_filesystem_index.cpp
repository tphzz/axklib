#include "image_filesystem_attributes.hpp"
#include "image_filesystem_internal.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <string_view>
#include <utility>

#include "axklib/filesystem_transaction.hpp"
#include "axklib/lookups.hpp"

namespace axk::app::detail {
namespace {
constexpr std::size_t maximum_entries = 1'000'000U;
constexpr std::size_t maximum_metadata_bytes = 64U * 1024U * 1024U;
constexpr std::size_t maximum_depth = 64U;

class Builder {
  public:
    ImageFilesystemIndex index;
    std::shared_ptr<const RandomAccessReader> source;
    std::map<std::pair<std::uint8_t, std::uint32_t>, std::vector<std::string>> record_objects;
    std::map<std::uint64_t, std::vector<std::string>> offset_objects;
    std::map<std::string, std::string> interpretations;
    std::map<std::pair<std::uint8_t, std::uint32_t>, std::string> volume_scopes;
    std::map<std::uint8_t, std::string> partition_scopes;
    bool exceeded{};
    std::size_t metadata_bytes{};

    void account_attributes(std::size_t position) {
        for (const auto &attribute : index.entries[position].attributes)
            metadata_bytes += sizeof(ImageFilesystemAttribute) + attribute.code.size() + attribute.label.size() +
                              attribute.value.size() + attribute.description.size() + attribute.summary.size();
        exceeded = exceeded || metadata_bytes > maximum_metadata_bytes;
    }

    std::size_t add(std::string id, std::string name, std::string kind, std::optional<std::size_t> parent = {}) {
        ImageFilesystemEntry entry;
        entry.id = std::move(id);
        entry.name = std::move(name);
        entry.kind = std::move(kind);
        if (parent) {
            auto &container = index.entries[*parent];
            entry.parent_id = container.id;
            entry.root_id = container.root_id;
            entry.ancestor_ids = container.ancestor_ids;
            entry.ancestor_ids.push_back(container.id);
            entry.path = container.path + "/" + entry.name;
            ++container.child_count;
        } else {
            entry.root_id = entry.id;
            index.root_capabilities.push_back({.root_id = entry.id});
        }
        metadata_bytes += sizeof(ImageFilesystemEntry) + 256U + entry.id.size() + entry.name.size() + entry.path.size();
        for (const auto &ancestor : entry.ancestor_ids)
            metadata_bytes += sizeof(std::string) + ancestor.size();
        exceeded = exceeded || metadata_bytes > maximum_metadata_bytes;
        index.entries.push_back(std::move(entry));
        return index.entries.size() - 1U;
    }

    void object(std::size_t position, const std::vector<std::string> &ids) {
        if (ids.size() != 1U)
            return;
        auto &entry = index.entries[position];
        entry.object_id = ids.front();
        entry.interpretation = interpretations.at(ids.front());
    }

    void sfs(const Container &container) {
        index.available = true;
        index.filesystem_name = "Yamaha SFS";
        for (const auto &partition : container.partitions()) {
            const auto root = add(std::format("sfs-{}", partition.index.value), partition.name, "partition");
            index.edit_partitions.emplace(index.entries[root].id, partition.index);
            if (axk::detail::inspect_sfs_file_edit_support(container, partition.index))
                index.root_capabilities.back() = {index.entries[root].id,
                                                  true,
                                                  true,
                                                  true,
                                                  true,
                                                  23U,
                                                  "^[ -~]{1,23}$",
                                                  "Use 1-23 printable ASCII characters."};
            if (const auto scope = partition_scopes.find(partition.index.value); scope != partition_scopes.end())
                index.entries[root].content_scope_id = scope->second;
            index.entries[root].storage =
                std::format("{} bytes per sector; {} sectors per cluster", container.superblock().sector_size_bytes,
                            partition.sectors_per_cluster);
            const auto root_record = locate_partition_root_record(partition);
            if (!root_record) {
                index.entries[root].issue = "Partition root directory is unresolved";
                continue;
            }
            std::map<std::uint32_t, const IndexRecord *> records;
            for (const auto &record : partition.records)
                records.emplace(record.sfs_id.value, &record);
            const auto found = records.find(root_record->value);
            if (found == records.end())
                continue;
            describe_sfs_attributes(index.entries[root], *found->second, partition,
                                    container.superblock().sector_size_bytes);
            account_attributes(root);
            struct Pending {
                std::size_t parent;
                const IndexRecord *record;
                std::set<std::uint32_t> ancestors;
            };
            std::vector<Pending> pending{{root, found->second, {root_record->value}}};
            while (!pending.empty()) {
                auto current = std::move(pending.back());
                pending.pop_back();
                for (const auto &stored : current.record->directory_entries) {
                    if (stored.state != DirectoryEntryState::live || stored.name == "." || stored.name == "..")
                        continue;
                    if (exceeded || index.entries.size() >= maximum_entries) {
                        exceeded = true;
                        return;
                    }
                    const auto target =
                        stored.target_link_id ? records.find(stored.target_link_id->value) : records.end();
                    const auto *record = target != records.end() ? target->second : nullptr;
                    const bool directory = record && record->payload_kind == PayloadKind::directory;
                    const auto position = add(std::format("sfs-{}-{}-{}-{}", partition.index.value, current.parent,
                                                          current.record->sfs_id.value, stored.payload_relative_offset),
                                              stored.name, directory ? "directory" : "file", current.parent);
                    auto &entry = index.entries[position];
                    entry.filesystem_metadata =
                        current.parent == root && (stored.name == "sfserrlog" || stored.name == "sfserram");
                    if (!record) {
                        if (current.parent != root || !is_partition_support_root_entry(stored.name))
                            entry.issue = "Directory entry target is unresolved";
                        continue;
                    }
                    entry.storage = std::format("Record {}; {} extents; {} allocated bytes", record->sfs_id.value,
                                                record->extents.size(), record->extent_byte_count_total);
                    describe_sfs_attributes(entry, *record, partition, container.superblock().sector_size_bytes);
                    account_attributes(position);
                    if (!directory) {
                        entry.size_bytes = record->data_size;
                        index.files.emplace(entry.id, SfsFilesystemFile{partition.index, record->sfs_id});
                        object(position, record_objects[{partition.index.value, record->sfs_id.value}]);
                        continue;
                    }
                    if (const auto scope = volume_scopes.find({partition.index.value, record->sfs_id.value});
                        scope != volume_scopes.end())
                        entry.content_scope_id = scope->second;
                    if (current.ancestors.contains(record->sfs_id.value)) {
                        entry.issue = "Directory cycle; traversal stopped";
                    } else if (entry.ancestor_ids.size() >= maximum_depth) {
                        entry.issue = "Directory depth limit reached";
                    } else {
                        auto ancestors = current.ancestors;
                        ancestors.insert(record->sfs_id.value);
                        pending.push_back({position, record, std::move(ancestors)});
                    }
                }
            }
        }
    }

    void files(const FatImage &fat, std::size_t member, bool map_objects, PartitionIndex partition) {
        if (fat.files().size() + fat.directories().size() + 1U > maximum_entries - index.entries.size()) {
            exceeded = true;
            return;
        }
        index.available = true;
        const bool ex5 =
            fat.geometry().profile == FatProfile::ex5_disk || fat.geometry().profile == FatProfile::ex5_removable;
        const bool fat16 = fat.geometry().profile == FatProfile::fat16;
        index.filesystem_name = ex5 ? "EX5 FAT16" : fat16 ? "FAT16" : "FAT12";
        const auto prefix = std::format("fat-{}", member);
        const auto root = add(prefix,
                              ex5     ? "EX5 disk"
                              : fat16 ? "FAT16 volume"
                                      : std::format("Floppy {}", member + 1U),
                              "root");
        index.entries[root].storage = std::format("{} bytes per sector; {} sectors per cluster",
                                                  fat.geometry().bytes_per_sector, fat.geometry().sectors_per_cluster);
        const auto declared_bytes =
            static_cast<std::uint64_t>(fat.geometry().total_sectors) * fat.geometry().bytes_per_sector;
        if (declared_bytes > fat.geometry().physical_size_bytes)
            index.entries[root].storage += std::format(
                "; declared {} bytes; physical {} bytes; {} unavailable bytes; {} incomplete data cluster(s)",
                declared_bytes, fat.geometry().physical_size_bytes, declared_bytes - fat.geometry().physical_size_bytes,
                fat.geometry().data_cluster_count - fat.geometry().backed_data_cluster_count);
        if ((ex5 || fat16) && source && axk::detail::inspect_fat_file_edit_support(source, partition)) {
            index.edit_partitions.emplace(index.entries[root].id, partition);
            index.root_capabilities.back() = {index.entries[root].id,
                                              true,
                                              true,
                                              true,
                                              true,
                                              12U,
                                              R"(^[A-Z0-9!#$%&'()@^_`{}~-]{1,8}(\.[A-Z0-9!#$%&'()@^_`{}~-]{1,3})?$)",
                                              "Use 1-8 ASCII characters plus an optional 1-3 character extension. "
                                              "Letters are uppercased automatically.",
                                              {},
                                              "FAT_8_3_UPPERCASE"};
        }
        std::map<std::string, std::size_t> directories{{"", root}};
        auto sorted = fat.directories();
        std::ranges::sort(sorted, {}, &FatDirectory::path);
        for (const auto &directory : sorted) {
            if (exceeded)
                return;
            const auto parent = directories.find(parent_path(directory.path));
            if (parent == directories.end())
                continue;
            const auto position = add(std::format("{}-d{}", prefix, directory.directory_offset), directory.name,
                                      "directory", parent->second);
            directories.emplace(directory.path, position);
            index.entries[position].storage = std::format("{} clusters", directory.clusters.size());
            describe_fat_attributes(index.entries[position], directory.attributes);
            account_attributes(position);
        }
        for (std::size_t ordinal = 0U; ordinal < fat.files().size(); ++ordinal) {
            const auto &file = fat.files()[ordinal];
            if (exceeded)
                return;
            const auto parent = directories.find(parent_path(file.path));
            if (parent == directories.end())
                continue;
            const auto position =
                add(std::format("{}-f{}", prefix, file.directory_offset), file.name, "file", parent->second);
            index.entries[position].size_bytes = file.size;
            index.files.emplace(index.entries[position].id, FatFilesystemFile{member, ordinal});
            index.entries[position].storage =
                std::format("First cluster {}; {} clusters", file.first_cluster, file.clusters.size());
            describe_fat_attributes(index.entries[position], file.attributes);
            account_attributes(position);
            if (map_objects)
                object(position, offset_objects[file.first_data_offset]);
        }
    }

    static std::string parent_path(std::string_view path) {
        const auto slash = path.find_last_of('/');
        return slash == std::string_view::npos ? std::string{} : std::string{path.substr(0, slash)};
    }

    void iso(const IsoImage &image) {
        if (image.files().size() + 1U > maximum_entries - index.entries.size()) {
            exceeded = true;
            return;
        }
        index.available = true;
        index.filesystem_name = "ISO9660";
        const auto root = add("iso-root", image.volume_id().empty() ? "CD-ROM" : image.volume_id(), "root");
        std::map<std::string, std::size_t> directories{{"", root}};
        std::vector<std::size_t> order(image.files().size());
        std::iota(order.begin(), order.end(), 0U);
        std::ranges::stable_sort(
            order, [&](auto left, auto right) { return image.files()[left].path < image.files()[right].path; });
        for (std::size_t ordinal = 0U; ordinal < order.size(); ++ordinal) {
            if (exceeded)
                return;
            const auto &file = image.files()[order[ordinal]];
            auto path = file.path;
            while (path.starts_with('/'))
                path.erase(0U, 1U);
            if (path.empty())
                continue;
            const auto parent = directories.find(parent_path(path));
            if (parent == directories.end())
                continue;
            const auto name =
                path.substr(path.find_last_of('/') == std::string::npos ? 0U : path.find_last_of('/') + 1U);
            const auto position =
                add(std::format("iso-{}", ordinal), name, file.is_directory ? "directory" : "file", parent->second);
            index.entries[position].storage = std::format("Extent sector {}", file.extent_sector);
            if (file.is_directory)
                directories.emplace(path, position);
            else {
                index.entries[position].size_bytes = file.size;
                index.files.emplace(index.entries[position].id, IsoFilesystemFile{order[ordinal]});
                object(position, offset_objects[static_cast<std::uint64_t>(file.extent_sector) * 2048U]);
            }
        }
    }
};
} // namespace

Result<ImageFilesystemIndex>
build_image_filesystem(const MediaContainer &media, std::shared_ptr<const RandomAccessReader> source,
                       const std::unordered_map<std::string, ObjectSnapshot> &objects,
                       const std::unordered_map<std::string, MediaObjectDescriptor> &descriptors,
                       const std::vector<ImageContentItem> &content) {
    Builder builder;
    builder.source = std::move(source);
    for (const auto &[id, object] : objects) {
        if (object.object.format != ObjectFormat::unknown)
            builder.index.device_view = "a-series";
        builder.record_objects[{object.partition.value, object.sfs_id.value}].push_back(id);
        builder.interpretations.emplace(id, object.object.header.raw_type);
    }
    for (const auto &[id, descriptor] : descriptors)
        builder.offset_objects[descriptor.data_offset].push_back(id);
    for (const auto &item : content) {
        if (!item.partition_index)
            continue;
        if (item.kind == "partition")
            builder.partition_scopes.emplace(*item.partition_index, item.id);
        if (item.kind == "volume" && item.volume_directory_id)
            builder.volume_scopes.emplace(std::pair{*item.partition_index, *item.volume_directory_id}, item.id);
    }
    if (const auto *sfs = std::get_if<Container>(&media.storage())) {
        builder.sfs(*sfs);
        // An empty SFS image can be populated with A-series volumes without an object discriminator.
        if (std::ranges::none_of(builder.index.entries, [](const auto &entry) {
                const bool partition_support =
                    entry.parent_id == entry.root_id && (entry.name == "sfserrlog" || entry.name == "sfserram");
                return entry.kind == "file" && !partition_support;
            }))
            builder.index.device_view = "a-series";
    } else if (const auto *fat = std::get_if<FatImage>(&media.storage()))
        builder.files(*fat, 0U, true, PartitionIndex{0});
    else if (const auto *disk = std::get_if<FatDiskImage>(&media.storage())) {
        for (std::size_t ordinal = 0; ordinal < disk->partitions().size(); ++ordinal) {
            const auto &partition = disk->partitions()[ordinal];
            const auto root = builder.index.entries.size();
            builder.files(partition.volume, partition.number, false,
                          PartitionIndex{static_cast<std::uint8_t>(ordinal)});
            if (root < builder.index.entries.size()) {
                builder.index.entries[root].name = std::format("Partition {}", partition.number);
                builder.index.entries[root].kind = "partition";
                builder.index.entries[root].storage += std::format("; offset {} bytes", partition.byte_offset);
            }
        }
    } else if (const auto *iso = std::get_if<IsoImage>(&media.storage()))
        builder.iso(*iso);
    else if (const auto *set = std::get_if<FloppyDiskSet>(&media.storage())) {
        for (std::size_t member = 0U; member < set->members().size(); ++member)
            builder.files(set->members()[member], member, false, PartitionIndex{0});
    }
    if (builder.exceeded || builder.index.entries.size() > maximum_entries)
        return std::unexpected(Error{"filesystem_limit_exceeded", "Filesystem entry or metadata budget exceeded"});
    identify_su700_import_roots(builder.index, media);
    std::map<std::string, std::size_t> mappings;
    for (const auto &entry : builder.index.entries)
        if (entry.object_id)
            ++mappings[*entry.object_id];
    for (auto &entry : builder.index.entries)
        if (entry.object_id && mappings[*entry.object_id] != 1U)
            entry.object_id.reset();
    return std::move(builder.index);
}
} // namespace axk::app::detail
