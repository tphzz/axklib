#include "axklib/native_bundle_media.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "axklib/capacity_admission_internal.hpp"
#include "axklib/writer_internal.hpp"

namespace axk::a_series::authoring {
Result<std::vector<std::byte>> build_hds(VolumeSpec volume, std::span<ImportedAudio> audio, const FreshMedia &target,
                                         const CancellationToken &cancellation) {
    if (target.partition_count < 1U || target.partition_count > 8U || target.partition_index >= target.partition_count)
        return std::unexpected(
            make_error(ErrorCode::invalid_argument, ErrorCategory::object, "Invalid SFS partition placement"));
    HdsBuildManifest manifest{std::string{build_manifest_schema_version}, target.size_bytes, {}};
    for (std::size_t i = 0; i < target.partition_count; ++i)
        manifest.partitions.push_back({"PARTITION " + std::to_string(i + 1U), {}});
    manifest.partitions[target.partition_index].volumes.push_back(std::move(volume));
    auto geometry = plan_hds_geometry(manifest);
    if (!geometry)
        return std::unexpected(geometry.error());
    std::vector<std::vector<detail::PreparedRecord>> records;
    for (std::size_t i = 0; i < geometry->size(); ++i) {
        auto prepared =
            detail::prepare_partition_records(manifest.partitions[i], geometry->at(i), geometry->size(), cancellation,
                                              i == target.partition_index ? std::optional{audio} : std::nullopt);
        if (!prepared)
            return std::unexpected(prepared.error());
        records.push_back(std::move(*prepared));
    }
    if (target.size_bytes > std::vector<std::byte>{}.max_size())
        return std::unexpected(
            make_error(ErrorCode::io_unsupported_size, ErrorCategory::io, "SFS image exceeds addressable memory"));
    std::vector<std::byte> bytes(static_cast<std::size_t>(target.size_bytes));
    const auto written = detail::emit_hds_records(
        manifest, *geometry, records, {},
        [&bytes](std::uint64_t offset, std::span<const std::byte> data) -> Result<void> {
            if (offset > bytes.size() || data.size() > bytes.size() - offset)
                return std::unexpected(make_error(ErrorCode::io_unsupported_size, ErrorCategory::io,
                                                  "SFS output exceeds its requested size"));
            std::ranges::copy(data, std::span{bytes}.subspan(static_cast<std::size_t>(offset)).begin());
            return {};
        },
        cancellation);
    if (!written)
        return std::unexpected(written.error());
    records.clear();
    OpenOptions options;
    options.cancellation = cancellation;
    const auto image = open_image(std::make_shared<MemoryReader>(bytes), {}, options);
    if (!image)
        return std::unexpected(image.error());
    const std::array destinations{
        detail::CapacityDestination{PartitionIndex{target.partition_index}, target.volume_name, std::nullopt}};
    const VolumeCapacityPolicy policy{};
    const auto admission = detail::inspect_capacity_destinations(*image, destinations, policy, cancellation);
    if (!admission)
        return std::unexpected(admission.error());
    if (auto ok = enforce_volume_capacity_admission(*admission, policy); !ok)
        return std::unexpected(ok.error());
    return bytes;
}
} // namespace axk::a_series::authoring
