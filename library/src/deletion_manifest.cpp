#include "axklib/deletion_manifest.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <vector>

#include "axklib/bytes.hpp"

namespace {

axk::Error deletion_error(std::string message) {
    return axk::make_error(axk::ErrorCode::transaction_rejected, axk::ErrorCategory::transaction, std::move(message));
}

int object_type_order(axk::ObjectType type) {
    switch (type) {
    case axk::ObjectType::prog:
        return 0;
    case axk::ObjectType::sbac:
        return 1;
    case axk::ObjectType::sbnk:
        return 2;
    case axk::ObjectType::smpl:
        return 3;
    case axk::ObjectType::sequ:
        return 4;
    default:
        return 5;
    }
}

std::optional<std::uint8_t> program_number(const axk::ObjectSnapshot &object) {
    unsigned value{};
    const auto &name = object.object.header.name;
    const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), value);
    if (error != std::errc{} || end != name.data() + name.size() || value == 0U || value > 128U)
        return std::nullopt;
    return static_cast<std::uint8_t>(value);
}

} // namespace

axk::Result<axk::detail::ObjectDeletionManifestPlan>
axk::detail::build_object_deletion_manifest(const Container &container, const ObjectCatalog &catalog,
                                            std::span<const ObjectDeletionImpact> impacts) {
    ObjectDeletionManifestPlan result;
    result.manifest.schema_version = std::string{alteration_manifest_schema_version};
    std::vector<const ObjectDeletionImpact *> ordered;
    for (const auto &impact : impacts) {
        if (impact.selected)
            ordered.push_back(&impact);
    }
    std::ranges::sort(ordered, {}, [](const auto *impact) {
        return std::tuple{impact->partition.value, impact->volume_name, object_type_order(impact->object_type),
                          impact->object_name, impact->object_key};
    });
    std::size_t program_index{};
    std::size_t bank_index{};
    std::size_t sample_index{};
    std::size_t wave_index{};
    std::size_t sequence_index{};
    for (const auto *impact : ordered) {
        const auto partition = std::ranges::find(container.partitions(), impact->partition, &Partition::index);
        if (partition == container.partitions().end())
            return std::unexpected(deletion_error("deletion impact partition is not available"));
        const auto cluster_size =
            checked_multiply(container.superblock().sector_size_bytes, partition->sectors_per_cluster);
        const auto freed_bytes = cluster_size ? checked_multiply(impact->freed_clusters, *cluster_size)
                                              : Result<std::uint64_t>{std::unexpected{cluster_size.error()}};
        const auto total_bytes = freed_bytes ? checked_add(result.estimated_freed_bytes, *freed_bytes)
                                             : Result<std::uint64_t>{std::unexpected{freed_bytes.error()}};
        const auto total_clusters = checked_add(result.estimated_freed_clusters, impact->freed_clusters);
        if (!total_bytes || !total_clusters)
            return std::unexpected(deletion_error("deletion recovery estimate exceeds supported limits"));
        result.estimated_freed_bytes = *total_bytes;
        result.estimated_freed_clusters = *total_clusters;

        const auto object = std::ranges::find(catalog.objects, impact->object_key, &ObjectSnapshot::key);
        if (object == catalog.objects.end())
            return std::unexpected(deletion_error("selected deletion object is not available"));
        if (impact->object_type == ObjectType::prog) {
            const auto number = program_number(*object);
            if (!number)
                return std::unexpected(deletion_error("selected Program number is unreadable"));
            result.manifest.operations.push_back(
                {std::format("delete-program-{}", ++program_index),
                 DeleteProgramOperation{impact->partition, impact->volume_name, *number}});
        } else if (impact->object_type == ObjectType::sbac) {
            result.manifest.operations.push_back(
                {std::format("delete-sample-bank-{}", ++bank_index),
                 DeleteSampleBankOperation{impact->partition, impact->volume_name, object->object.header.name}});
        } else if (impact->object_type == ObjectType::sbnk) {
            result.manifest.operations.push_back(
                {std::format("delete-sample-{}", ++sample_index),
                 DeleteSampleOperation{impact->partition, impact->volume_name, object->object.header.name}});
        } else if (impact->object_type == ObjectType::smpl) {
            result.manifest.operations.push_back(
                {std::format("delete-wave-data-{}", ++wave_index),
                 DeleteWaveformOperation{impact->partition, impact->volume_name, object->object.header.name}});
        } else {
            result.manifest.operations.push_back(
                {std::format("delete-sequence-{}", ++sequence_index),
                 DeleteSequenceOperation{impact->partition, impact->volume_name, object->object.header.name}});
        }
    }
    return result;
}
