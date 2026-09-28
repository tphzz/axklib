#include "alteration_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <vector>

#include "axklib/bytes.hpp"
#include "axklib/writer_internal.hpp"

namespace axk::alteration_internal {

Result<std::vector<std::byte>> encode_changed_record_index(const MutablePartition::InsertedRecord &record,
                                                           std::span<const Extent> extents, std::size_t size) {
    if (size > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected{transaction_error("record payload exceeds its encoded size")};
    detail::PreparedRecord prepared;
    auto encoded = detail::encode_sfs_index_record(prepared, extents, static_cast<std::uint32_t>(size),
                                                   record.continuation_clusters);
    if (!encoded)
        return std::unexpected{encoded.error()};
    if (record.raw_index.size() != encoded->size())
        return std::unexpected{transaction_error("changed SFS index record has an invalid size")};
    auto result = record.raw_index;
    // Replace only allocation fields. Preserve flags and opaque directory/object metadata.
    std::copy_n(encoded->begin(), 2U, result.begin());
    std::copy(encoded->begin() + 4, encoded->begin() + 0x3a, result.begin() + 4);
    return result;
}

Result<std::uint64_t> shrink_record_extents(MutablePartition &partition, MutablePartition::InsertedRecord &record,
                                            std::size_t size) {
    if (size == 0U)
        return std::unexpected{transaction_error("SFS record payload must not be empty")};
    std::uint64_t capacity{};
    std::size_t retained{};
    while (retained < record.extents.size() && capacity < size) {
        capacity += static_cast<std::uint64_t>(record.extents[retained].cluster_count) * 1024U;
        ++retained;
    }
    if (capacity < size)
        return std::unexpected{transaction_error("record payload exceeds its extent capacity")};
    if (retained == record.extents.size())
        return 0U;
    constexpr std::size_t extents_per_list = (1024U - 12U) / 12U;
    const auto lists = retained <= 4U ? 0U : (retained + extents_per_list - 1U) / extents_per_list;
    if (lists > record.continuation_clusters.size())
        return std::unexpected{transaction_error("record has insufficient continuation lists")};
    std::uint64_t freed{};
    for (const auto &extent : std::span{record.extents}.subspan(retained)) {
        for (std::uint32_t index = 0; index < extent.cluster_count; ++index)
            set_bitmap(partition.bitmap, extent.cluster_offset + index, false);
        freed += extent.cluster_count;
    }
    for (const auto cluster : std::span{record.continuation_clusters}.subspan(lists)) {
        set_bitmap(partition.bitmap, cluster, false);
        ++freed;
    }
    record.extents.resize(retained);
    record.continuation_clusters.resize(lists);
    return freed;
}

} // namespace axk::alteration_internal
