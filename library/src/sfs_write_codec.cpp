#include "axklib/writer_internal.hpp"

#include <algorithm>
#include <limits>

#include "axklib/bytes.hpp"

namespace axk::detail {
namespace {

Error encoding_error(std::string message) {
    return make_error(ErrorCode::internal_invariant, ErrorCategory::internal, std::move(message));
}

Result<void> write_record_header(ByteWriter &writer, std::size_t extent_count, std::uint32_t clusters,
                                 std::uint32_t size) {
    if (extent_count > std::numeric_limits<std::uint16_t>::max() ||
        clusters > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected{encoding_error("SFS index record values exceed their encoded width")};
    }
    if (auto written = writer.write_be16(0U, static_cast<std::uint16_t>(extent_count)); !written)
        return written;
    if (auto written = writer.write_be16(4U, static_cast<std::uint16_t>(clusters)); !written)
        return written;
    return writer.write_be32(6U, size);
}

Result<void> write_record_kind(ByteWriter &writer, RecordKind kind, std::uint16_t tail) {
    if (kind == RecordKind::object) {
        if (auto written = writer.write_u8(0x42U, 0x9eU); !written)
            return written;
        return writer.write_u8(0x47U, 1U);
    }
    if (auto written = writer.write_u8(0x42U, 0x94U); !written)
        return written;
    if (kind == RecordKind::directory) {
        if (auto written = writer.write_ascii_field(0x43U, 3U, "dir", std::byte{0}); !written)
            return written;
        return writer.write_be16(0x46U, tail);
    }
    return writer.write_u8(0x47U, 1U);
}

} // namespace

Result<std::vector<std::byte>> encode_sfs_index_record(const PreparedRecord &record) {
    std::vector<std::byte> result(sfs_directory_index_record_bytes);
    ByteWriter writer{result};
    if (record.kind != RecordKind::system) {
        if (record.payload.size() > std::numeric_limits<std::uint32_t>::max())
            return std::unexpected{encoding_error("SFS record payload exceeds its encoded size")};
        if (auto written =
                write_record_header(writer, 1U, record.clusters, static_cast<std::uint32_t>(record.payload.size()));
            !written) {
            return std::unexpected{written.error()};
        }
        if (auto written = writer.write_be32(0x0aU, record.cluster); !written)
            return std::unexpected{written.error()};
        if (auto written = writer.write_be32(0x0eU, record.clusters); !written)
            return std::unexpected{written.error()};
        if (auto written = writer.write_be32(0x12U, static_cast<std::uint32_t>(record.payload.size())); !written)
            return std::unexpected{written.error()};
    }
    std::fill(result.begin() + 0x3a, result.begin() + 0x42, std::byte{0xff});
    if (auto written = write_record_kind(writer, record.kind, record.tail); !written)
        return std::unexpected{written.error()};
    return result;
}

Result<std::vector<std::byte>> encode_sfs_index_record(const PreparedRecord &record, std::span<const Extent> extents,
                                                       std::uint32_t size,
                                                       std::span<const std::uint32_t> continuation_clusters) {
    if (extents.empty())
        return std::unexpected{encoding_error("SFS record requires at least one extent")};
    if (continuation_clusters.empty() && extents.size() > 4U)
        return std::unexpected{encoding_error("direct SFS index record cannot encode more than four extents")};

    std::uint64_t total_clusters{};
    for (const auto &extent : extents)
        total_clusters += extent.cluster_count;
    if (total_clusters > std::numeric_limits<std::uint16_t>::max())
        return std::unexpected{encoding_error("SFS record cluster count exceeds its encoded width")};
    const auto clusters = static_cast<std::uint32_t>(total_clusters);

    std::vector<std::byte> result(sfs_directory_index_record_bytes);
    ByteWriter writer{result};
    if (auto written = write_record_header(writer, extents.size(), clusters, size); !written)
        return std::unexpected{written.error()};

    if (!continuation_clusters.empty()) {
        if (auto written = writer.write_be32(0x0aU, continuation_clusters.front()); !written)
            return std::unexpected{written.error()};
        if (auto written = writer.write_be32(0x0eU, clusters); !written)
            return std::unexpected{written.error()};
        if (auto written = writer.write_be32(0x12U, size); !written)
            return std::unexpected{written.error()};
    } else {
        std::uint32_t remaining = size;
        for (std::size_t index = 0; index < extents.size(); ++index) {
            const auto &extent = extents[index];
            const auto capacity = static_cast<std::uint64_t>(extent.cluster_count) * 1024U;
            const auto byte_count =
                static_cast<std::uint32_t>(std::min<std::uint64_t>(remaining == 0U ? capacity : remaining, capacity));
            remaining = remaining > byte_count ? remaining - byte_count : 0U;
            const auto offset = 0x0aU + index * 12U;
            if (auto written = writer.write_be32(offset, extent.cluster_offset); !written)
                return std::unexpected{written.error()};
            if (auto written = writer.write_be32(offset + 4U, extent.cluster_count); !written)
                return std::unexpected{written.error()};
            if (auto written = writer.write_be32(offset + 8U, byte_count); !written)
                return std::unexpected{written.error()};
        }
    }
    std::fill(result.begin() + 0x3a, result.begin() + 0x42, std::byte{0xff});
    if (auto written = write_record_kind(writer, record.kind, record.tail); !written)
        return std::unexpected{written.error()};
    return result;
}

} // namespace axk::detail
