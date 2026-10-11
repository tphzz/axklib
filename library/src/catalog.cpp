#include "axklib/catalog.hpp"

#include <algorithm>
#include <format>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "axklib/bytes.hpp"
#include "axklib/catalog_internal.hpp"
#include "axklib/system_file.hpp"

namespace axk {
namespace {

bool is_category(std::string_view name) {
    static const std::unordered_set<std::string_view> categories{"SMPL", "SBNK", "SBAC", "PROG", "SEQU", "PRF3"};
    return categories.contains(name);
}

std::string object_key(PartitionIndex partition, SfsId sfs_id) {
    return std::format("p{}:sfs{}", partition.value, sfs_id.value);
}

Result<std::vector<std::byte>> read_object(const Container &container, const Partition &partition,
                                           const IndexRecord &record, std::size_t maximum_bytes,
                                           const CancellationToken &cancellation, MediaObjectReadMode mode) {
    if (mode == MediaObjectReadMode::complete || record.data_size > maximum_bytes)
        return container.read_record_data(partition.index, record.sfs_id, maximum_bytes, cancellation);
    const auto prefix_size = static_cast<std::size_t>(std::min<std::uint64_t>(record.data_size, 0xacU));
    auto bytes = container.read_record_range(partition.index, record.sfs_id, 0U, prefix_size, cancellation);
    if (!bytes)
        return std::unexpected{bytes.error()};
    const auto header = decode_object_header(*bytes);
    if (!header || header->type != ObjectType::smpl) {
        auto remaining =
            container.read_record_range(partition.index, record.sfs_id, prefix_size,
                                        static_cast<std::size_t>(record.data_size) - prefix_size, cancellation);
        if (!remaining)
            return std::unexpected{remaining.error()};
        bytes->insert(bytes->end(), remaining->begin(), remaining->end());
        return bytes;
    }
    // Validate the complete allocation without fetching PCM. A valid prefix alone
    // must not conceal a truncated or out-of-range later extent.
    std::uint64_t available{};
    for (const auto &extent : record.extents) {
        const auto capacity = checked_multiply(extent.cluster_count,
                                               static_cast<std::uint64_t>(container.superblock().sector_size_bytes) *
                                                   partition.sectors_per_cluster);
        if (!capacity || extent.byte_count > *capacity || extent.cluster_offset >= partition.cluster_count ||
            extent.cluster_count > partition.cluster_count - extent.cluster_offset)
            return std::unexpected{make_error(ErrorCode::allocation_invalid_extent, ErrorCategory::allocation,
                                              "data extent exceeds its allocation or partition")};
        available += extent.byte_count;
    }
    if (available < record.data_size)
        return std::unexpected{make_error(ErrorCode::io_short_read, ErrorCategory::io,
                                          "logical SFS record read did not produce its declared size")};
    return bytes;
}

} // namespace

Result<ObjectCatalog> detail::build_object_catalog(const Container &container, std::size_t maximum_object_bytes,
                                                   const CancellationToken &cancellation, MediaObjectReadMode mode,
                                                   ProgressSink *progress) {
    ObjectCatalog result;
    std::uint64_t completed{};
    std::uint64_t total{};
    for (const auto &partition : container.partitions()) {
        for (const auto &record : partition.records)
            total += record.payload_kind == PayloadKind::object ? 1U : 0U;
    }
    const auto report = [&] {
        if (progress && (completed == 0U || completed == total || completed % 16U == 0U))
            progress->report({ProgressPhase::reading, completed, total, "Reading object metadata", {}});
    };
    report();
    for (const auto &partition : container.partitions()) {
        if (const auto check = cancellation.check(); !check) {
            return std::unexpected{check.error()};
        }

        // Build placement candidates directly so directory ambiguity cannot be
        // hidden by a last-write-wins map.
        struct Candidate {
            SfsId target;
            ObjectPlacement placement;
        };
        std::vector<Candidate> candidates;
        std::unordered_map<std::uint32_t, const IndexRecord *> directories;
        for (const auto &record : partition.records) {
            if (record.directory_id)
                directories.emplace(record.directory_id->value, &record);
        }
        const auto located_root = locate_partition_root_record(partition);
        if (located_root) {
            const auto root_record = std::ranges::find(partition.records, *located_root, &IndexRecord::sfs_id);
            if (root_record == partition.records.end())
                continue;
            const auto *root = &*root_record;
            for (const auto &volume_entry : root->directory_entries) {
                if (!volume_entry.target_link_id)
                    continue;
                const auto volume_found = directories.find(volume_entry.target_link_id->value);
                if (volume_entry.name == "." || volume_entry.name == ".." ||
                    is_partition_support_root_entry(volume_entry.name) || volume_found == directories.end()) {
                    continue;
                }
                const auto *volume = volume_found->second;
                if (!volume->parent_directory_id || volume->parent_directory_id->value != root->directory_id->value) {
                    continue;
                }
                for (const auto &category_entry : volume->directory_entries) {
                    if (!category_entry.target_link_id || !is_category(category_entry.name))
                        continue;
                    const auto category_found = directories.find(category_entry.target_link_id->value);
                    if (category_found == directories.end())
                        continue;
                    const auto *category = category_found->second;
                    if (!category->parent_directory_id || !volume->directory_id ||
                        category->parent_directory_id->value != volume->directory_id->value) {
                        continue;
                    }
                    for (const auto &entry : category->directory_entries) {
                        if (entry.name == "." || entry.name == ".." || !entry.target_link_id)
                            continue;
                        candidates.push_back({
                            SfsId{entry.target_link_id->value},
                            {partition.index,
                             partition.name,
                             volume->sfs_id,
                             volume_entry.name,
                             category_entry.name,
                             entry.name,
                             {}},
                        });
                    }
                }
            }
        }

        std::unordered_set<std::uint32_t> partition_support_records;
        for (const auto kind : {SystemFileKind::a3000_system, SystemFileKind::a4000_a5000_system2}) {
            const auto located = locate_system_file_record(partition, kind);
            if (located && *located)
                partition_support_records.insert((**located).value);
        }

        for (const auto &record : partition.records) {
            if (record.payload_kind != PayloadKind::object)
                continue;
            ++completed;
            if (partition_support_records.contains(record.sfs_id.value)) {
                report();
                continue;
            }
            auto bytes = read_object(container, partition, record, maximum_object_bytes, cancellation, mode);
            if (!bytes)
                return std::unexpected{bytes.error()};
            auto decoded = decode_object(*bytes);
            if (!decoded) {
                result.issues.push_back({
                    "CATALOG_OBJECT_DECODE_FAILED",
                    decoded.error().message,
                    partition.index,
                    record.sfs_id,
                });
                auto header = decode_object_header(*bytes);
                if (!header || (header->type != ObjectType::sequ && header->type != ObjectType::prog)) {
                    report();
                    continue;
                }
                decoded = DecodedObject{std::move(*header), ObjectFormat::unknown,
                                        GenericObject{std::vector<std::byte>{bytes->begin(), bytes->end()}}};
            }
            std::vector<ObjectPlacement> matching;
            for (const auto &candidate : candidates) {
                if (candidate.target.value == record.sfs_id.value)
                    matching.push_back(candidate.placement);
            }
            std::optional<ObjectPlacement> placement;
            auto resolution = PlacementResolution::missing;
            if (matching.size() == 1) {
                placement = matching.front();
                resolution = PlacementResolution::exact;
            } else {
                if (!matching.empty())
                    resolution = PlacementResolution::ambiguous;
                result.issues.push_back({
                    matching.empty() ? "CATALOG_OBJECT_PLACEMENT_MISSING" : "CATALOG_OBJECT_PLACEMENT_AMBIGUOUS",
                    matching.empty() ? "object has no exact volume/category "
                                       "directory placement"
                                     : "object has multiple volume/category "
                                       "directory placements",
                    partition.index,
                    record.sfs_id,
                });
            }
            auto raw_payload = mode == MediaObjectReadMode::complete ? std::move(*bytes) : std::vector<std::byte>{};
            result.objects.push_back({object_key(partition.index, record.sfs_id), partition.index, record.sfs_id,
                                      std::format("partition:{}", partition.index.value), *decoded,
                                      std::move(placement), std::move(raw_payload), std::move(matching), resolution});
            report();
        }
    }
    std::ranges::sort(result.objects, {},
                      [](const ObjectSnapshot &item) { return std::pair{item.partition.value, item.sfs_id.value}; });
    return result;
}

Result<ObjectCatalog> build_object_catalog(const Container &container, std::size_t maximum_object_bytes,
                                           const CancellationToken &cancellation) {
    return detail::build_object_catalog(container, maximum_object_bytes, cancellation, MediaObjectReadMode::complete);
}

std::string_view placement_resolution_name(PlacementResolution resolution) noexcept {
    switch (resolution) {
    case PlacementResolution::exact:
        return "exact";
    case PlacementResolution::missing:
        return "missing";
    case PlacementResolution::ambiguous:
        return "ambiguous";
    }
    return "missing";
}

} // namespace axk
