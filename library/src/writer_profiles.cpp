#include "axklib/writer.hpp"

#include <algorithm>
#include <array>
#include <tuple>

namespace axk {
namespace {

Error profile_error(std::string message) {
    return make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest, std::move(message));
}

} // namespace

Result<std::vector<PartitionGeometry>> plan_hds_geometry(const HdsBuildManifest &manifest) {
    if (manifest.partitions.empty() || manifest.partitions.size() > 8U || manifest.size_bytes < minimum_hds_size ||
        manifest.size_bytes > maximum_hds_size || manifest.size_bytes % 512U != 0U) {
        return std::unexpected{profile_error("HDS manifest geometry is outside the writer profile")};
    }
    constexpr std::uint64_t start = 3;
    constexpr std::uint64_t maximum_slot = 1'073'741'824 / 512U - 1U;
    const auto slot = std::min((manifest.size_bytes / 512U - (start - 1U)) / manifest.partitions.size(), maximum_slot);
    const auto sectors = slot - 1U;
    if (sectors < 2045U)
        return std::unexpected{profile_error("partition slots are too small for SFS")};
    const auto clusters = sectors / 2U;
    const auto bitmap_count = ((clusters + 7U) / 8U + 1023U) / 1024U;
    std::vector<PartitionGeometry> result;
    for (std::size_t index = 0; index < manifest.partitions.size(); ++index) {
        const auto bitmap = 2U + bitmap_count;
        const auto directory = bitmap + bitmap_count;
        result.push_back({static_cast<std::uint8_t>(index), start + index * slot, slot, sectors, clusters, bitmap,
                          bitmap_count, directory, directory + 358U});
    }
    return result;
}

std::string_view hds_creation_profile_id(HdsCreationProfileId id) {
    switch (id) {
    case HdsCreationProfileId::floppy_scale:
        return "floppy-scale";
    case HdsCreationProfileId::cd_r_650:
        return "cd-r-650";
    case HdsCreationProfileId::cd_r_700:
        return "cd-r-700";
    case HdsCreationProfileId::hds_1_gib:
        return "hds-1-gib";
    case HdsCreationProfileId::hds_2_gib:
        return "hds-2-gib";
    }
    return {};
}

Result<HdsCreationProfileId> parse_hds_creation_profile_id(std::string_view id) {
    constexpr std::array ids{HdsCreationProfileId::floppy_scale, HdsCreationProfileId::cd_r_650,
                             HdsCreationProfileId::cd_r_700, HdsCreationProfileId::hds_1_gib,
                             HdsCreationProfileId::hds_2_gib};
    const auto found = std::ranges::find(ids, id, hds_creation_profile_id);
    if (found == ids.end())
        return std::unexpected{profile_error("unknown HDS creation profile")};
    return *found;
}

const std::vector<HdsCreationProfile> &hds_creation_profiles() {
    static const auto profiles = [] {
        constexpr std::array definitions{
            std::tuple{HdsCreationProfileId::floppy_scale, 1'474'560ULL, std::uint8_t{1}},
            std::tuple{HdsCreationProfileId::cd_r_650, 333'000ULL * 2'048ULL, std::uint8_t{1}},
            std::tuple{HdsCreationProfileId::cd_r_700, 360'000ULL * 2'048ULL, std::uint8_t{1}},
            std::tuple{HdsCreationProfileId::hds_1_gib, 1'073'741'824ULL, std::uint8_t{1}},
            std::tuple{HdsCreationProfileId::hds_2_gib, 2'147'483'648ULL, std::uint8_t{2}},
        };
        std::vector<HdsCreationProfile> result;
        for (const auto &[id, size_bytes, default_partition_count] : definitions) {
            HdsCreationProfile profile{id, size_bytes, default_partition_count, {}};
            for (std::uint8_t count = 1; count <= 8; ++count) {
                HdsBuildManifest manifest{std::string{build_manifest_schema_version}, size_bytes, {}};
                for (std::uint8_t index = 0; index < count; ++index)
                    manifest.partitions.push_back({"PARTITION " + std::to_string(index + 1U), {}});
                auto geometry = plan_hds_geometry(manifest);
                if (!geometry)
                    continue;
                const auto &last = geometry->back();
                const auto unused_tail_sectors = size_bytes / 512U - (last.start_sector + last.filesystem_sector_count);
                if (unused_tail_sectors >= last.slot_sector_count)
                    continue;
                profile.partition_options.push_back({count, std::move(*geometry), unused_tail_sectors});
            }
            result.push_back(std::move(profile));
        }
        return result;
    }();
    return profiles;
}

Result<HdsCreationPlan> plan_hds_creation(const HdsCreationRequest &request, const CancellationToken &cancellation) {
    const auto &profiles = hds_creation_profiles();
    const auto profile = std::ranges::find(profiles, request.profile_id, &HdsCreationProfile::id);
    if (profile == profiles.end())
        return std::unexpected{profile_error("unknown HDS creation profile")};
    const auto option = std::ranges::find(profile->partition_options, request.partition_count,
                                          &HdsCreationPartitionOption::partition_count);
    if (option == profile->partition_options.end())
        return std::unexpected{profile_error("partition count is not available for the HDS creation profile")};

    HdsBuildManifest manifest{std::string{build_manifest_schema_version}, profile->size_bytes, {}};
    for (std::uint8_t index = 0; index < request.partition_count; ++index)
        manifest.partitions.push_back({"PARTITION " + std::to_string(index + 1U), {}});
    auto summary = plan_hds_build(manifest, cancellation);
    if (!summary)
        return std::unexpected{summary.error()};
    return HdsCreationPlan{request.profile_id, std::move(manifest), std::move(*summary), option->unused_tail_sectors};
}

} // namespace axk
