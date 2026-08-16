#include "image_sessions_internal.hpp"

#include <format>
#include <ranges>

axk::app::Result<axk::app::ImageAllocationMap>
axk::app::ImageSessionManager::allocation_map(std::string_view image_id, std::string_view owner_id,
                                              std::uint64_t expected_revision, std::uint8_t partition_index) {
    const auto read = begin_read(image_id, owner_id, expected_revision);
    if (!read)
        return std::unexpected(read.error());
    const auto *container = std::get_if<axk::Container>(&read->media->storage());
    if (container == nullptr) {
        return std::unexpected(session_error("allocation_map_unsupported",
                                             "partition allocation maps are available only for SFS hard-disk images"));
    }
    const auto partition =
        std::ranges::find(container->partitions(), axk::PartitionIndex{partition_index}, &axk::Partition::index);
    if (partition == container->partitions().end()) {
        return std::unexpected(
            session_error("partition_not_found", std::format("partition {} does not exist", partition_index)));
    }

    std::unordered_map<std::string, std::string> object_ids_by_key;
    object_ids_by_key.reserve(read->object_keys_by_id.size());
    for (const auto &[id, key] : read->object_keys_by_id)
        object_ids_by_key.emplace(key, id);
    std::unordered_map<std::uint32_t, AllocationObjectIdentity> objects_by_sfs_id;
    for (const auto *snapshot : read->catalog_objects) {
        if (snapshot->partition.value != partition_index)
            continue;
        const auto id = object_ids_by_key.find(snapshot->key);
        if (id == object_ids_by_key.end())
            continue;
        AllocationObjectIdentity identity{id->second,
                                          image_sessions_internal::object_type_name(snapshot->object.header.type),
                                          snapshot->object.header.name,
                                          {},
                                          {}};
        if (snapshot->placement) {
            identity.volume_name = snapshot->placement->volume_name;
            identity.category_name = snapshot->placement->category_name;
        }
        objects_by_sfs_id.emplace(snapshot->sfs_id.value, std::move(identity));
    }
    auto map = build_image_allocation_map(*partition, objects_by_sfs_id, container->superblock().sector_size_bytes);
    if (!map)
        return std::unexpected(core_error(map.error(), read->source));
    return *std::move(map);
}
