#include "server_application.hpp"

#include <limits>
#include <utility>

#include "server_support.hpp"

namespace axk::server::detail {
namespace {

Json owner_json(const app::ImageAllocationOwner &owner) {
    return {{"claimKind", owner.claim_kind},
            {"sfsId", owner.sfs_id ? Json(*owner.sfs_id) : Json{}},
            {"extentIndex", owner.extent_index ? Json(*owner.extent_index) : Json{}},
            {"recordKind", owner.record_kind},
            {"objectId", owner.object_id ? Json(*owner.object_id) : Json{}},
            {"objectType", owner.object_type},
            {"objectName", owner.object_name},
            {"volumeName", owner.volume_name},
            {"categoryName", owner.category_name}};
}

Json summary_json(const app::ImageAllocationSummary &summary) {
    return {{"totalClusters", summary.total_clusters},
            {"reservedClusters", summary.reserved_clusters},
            {"dataClusters", summary.data_clusters},
            {"continuationClusters", summary.continuation_clusters},
            {"freeClusters", summary.free_clusters},
            {"freeRunCount", summary.free_run_count},
            {"largestFreeRunClusters", summary.largest_free_run_clusters},
            {"allocatedClusters", summary.allocated_clusters},
            {"allocatedBytes", summary.allocated_bytes},
            {"logicalRecordBytes", summary.logical_record_bytes},
            {"dataSlackBytes", summary.data_slack_bytes},
            {"recordCount", summary.record_count},
            {"fragmentedRecordCount", summary.fragmented_record_count},
            {"totalExtentCount", summary.total_extent_count},
            {"maximumExtentCount", summary.maximum_extent_count},
            {"bitmapCopyMismatchClusters", summary.bitmap_copy_mismatch_clusters},
            {"claimedButFreeClusters", summary.claimed_but_free_clusters},
            {"usedWithoutClaimClusters", summary.used_without_claim_clusters},
            {"conflictingClusters", summary.conflicting_clusters},
            {"invalidExtentRecords", summary.invalid_extent_records}};
}

Json map_json(const app::ImageAllocationMap &map, std::string_view image_id, std::uint64_t revision) {
    Json runs = Json::array();
    for (const auto &run : map.runs) {
        Json owners = Json::array();
        for (const auto &owner : run.owners)
            owners.push_back(owner_json(owner));
        runs.push_back({{"startCluster", run.start_cluster},
                        {"clusterCount", run.cluster_count},
                        {"startSector", run.start_sector},
                        {"sectorCount", run.sector_count},
                        {"byteOffset", run.byte_offset},
                        {"byteCount", run.byte_count},
                        {"fixedBitmapUsed", run.fixed_bitmap_used},
                        {"headerBitmapUsed", run.header_bitmap_used},
                        {"reconstructedUsed", run.reconstructed_used},
                        {"allocationKind", run.allocation_kind},
                        {"owners", std::move(owners)},
                        {"consistencyFlags", run.consistency_flags}});
    }
    return {{"imageId", image_id},
            {"revision", revision},
            {"partitionIndex", map.partition_index},
            {"partitionName", map.partition_name},
            {"sectorSizeBytes", map.sector_size_bytes},
            {"sectorsPerCluster", map.sectors_per_cluster},
            {"clusterSizeBytes", map.cluster_size_bytes},
            {"clusterCount", map.cluster_count},
            {"partitionStartSector", map.partition_start_sector},
            {"summary", summary_json(map.summary)},
            {"runs", std::move(runs)}};
}

} // namespace

crow::response ServerApplication::image_allocation_map_response(const crow::request &request,
                                                                const std::string &image_id) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto partition = parse_unsigned(
        request.url_params.get("partitionIndex") == nullptr ? "" : request.url_params.get("partitionIndex"));
    const auto revision = parse_unsigned(
        request.url_params.get("expectedRevision") == nullptr ? "" : request.url_params.get("expectedRevision"));
    if (!partition || *partition > 7U || !revision) {
        return error_response(
            400,
            {"invalid_allocation_map_request", "partitionIndex from 0 through 7 and expectedRevision are required"},
            id);
    }
    const auto map =
        images_.allocation_map(image_id, request_owner(request), *revision, static_cast<std::uint8_t>(*partition));
    if (!map)
        return error_response(status_for_error(map.error()), map.error(), id);
    return json_response(200, {{"data", map_json(*map, image_id, *revision)}, {"meta", {{"requestId", id}}}}, id);
}

} // namespace axk::server::detail
