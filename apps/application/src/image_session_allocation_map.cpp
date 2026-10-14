#include "formats/a_series/session_state.hpp"
#include "formats/a_series/support.hpp"
#include "image_sessions_internal.hpp"

#include <format>
#include <ranges>

axk::app::Result<axk::app::ImageAllocationMap>
axk::app::ImageSessionManager::allocation_map(const ImageSourceTarget &target, std::string_view owner_id,
                                              std::uint64_t expected_revision, std::uint8_t partition_index) {
    const auto session = implementation_->owned(target.image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const auto read = begin_source_read(target, owner_id, expected_revision);
    if (!read)
        return std::unexpected(read.error());
    const auto *container = read->media ? std::get_if<axk::Container>(&read->media->storage()) : nullptr;
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

    std::unordered_map<std::uint32_t, AllocationObjectIdentity> objects_by_sfs_id;
    for (const auto &document : (*session)->documents) {
        if (!document->primary || document->primary->id != target.member_id)
            continue;
        const auto *native = dynamic_cast<const formats::a_series::SessionState *>(document->native.get());
        if (!native)
            continue;
        for (const auto &[id, snapshot] : native->snapshots_by_id) {
            if (snapshot->partition.value != partition_index)
                continue;
            AllocationObjectIdentity identity{id,
                                              formats::a_series::object_type_name(snapshot->object.header.type),
                                              snapshot->object.header.name,
                                              {},
                                              {}};
            if (snapshot->placement) {
                identity.volume_name = snapshot->placement->volume_name;
                identity.category_name = snapshot->placement->category_name;
            }
            objects_by_sfs_id.emplace(snapshot->sfs_id.value, std::move(identity));
        }
    }
    auto map = build_image_allocation_map(*partition, objects_by_sfs_id, container->superblock().sector_size_bytes);
    if (!map)
        return std::unexpected(core_error(map.error(), read->source));
    return *std::move(map);
}
