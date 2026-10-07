#include "media_smpl_segments.hpp"

#include <algorithm>
#include <tuple>

#include "axklib/bytes.hpp"
#include "media_internal.hpp"

namespace axk::detail {
std::vector<std::byte> smpl_segment_identity(const MediaObject &object) {
    const auto header_size = static_cast<std::size_t>(object.decoded.header.header_size);
    if (header_size > object.raw_payload.size())
        return {};
    std::vector<std::byte> identity{object.raw_payload.begin(),
                                    object.raw_payload.begin() + static_cast<std::ptrdiff_t>(header_size)};
    if (identity.size() < 0x28U)
        return {};
    std::fill(identity.begin() + 0x20, identity.begin() + 0x28, std::byte{});
    return identity;
}

Result<bool> assemble_smpl_segment_group(std::vector<MediaObject> &objects, std::vector<std::size_t> &indices,
                                         const CancellationToken &cancellation) {
    if (indices.empty())
        return false;
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected(checked.error());
    std::ranges::sort(indices, [&](std::size_t left, std::size_t right) {
        const auto &left_header = objects[left].decoded.header;
        const auto &right_header = objects[right].decoded.header;
        return std::tie(left_header.payload_offset_0x24, objects[left].logical_path) <
               std::tie(right_header.payload_offset_0x24, objects[right].logical_path);
    });
    const auto &first_header = objects[indices.front()].decoded.header;
    if (first_header.payload_bytes_0x1c > 64U * 1024U * 1024U)
        return std::unexpected(
            make_error(ErrorCode::io_unsupported_size, ErrorCategory::io, "Wave Data exceeds the assembly limit."));
    std::uint64_t covered{};
    std::vector<std::size_t> unique_indices;
    for (const auto index : indices) {
        if (const auto checked = cancellation.check(); !checked)
            return std::unexpected(checked.error());
        const auto &object = objects[index];
        const auto &header = object.decoded.header;
        if (header.header_size != first_header.header_size ||
            header.payload_bytes_0x1c != first_header.payload_bytes_0x1c) {
            return false;
        }
        const auto local_end = checked_add(header.header_size, header.payload_bytes_0x20);
        const auto logical_end = checked_add(header.payload_offset_0x24, header.payload_bytes_0x20);
        if (!local_end || *local_end > object.raw_payload.size() || !logical_end ||
            *logical_end > header.payload_bytes_0x1c) {
            return false;
        }
        if (header.payload_offset_0x24 < covered) {
            const auto duplicate = std::ranges::find_if(unique_indices, [&](std::size_t previous) {
                const auto &candidate = objects[previous];
                const auto &candidate_header = candidate.decoded.header;
                if (candidate_header.payload_offset_0x24 != header.payload_offset_0x24 ||
                    candidate_header.payload_bytes_0x20 != header.payload_bytes_0x20) {
                    return false;
                }
                const auto candidate_pcm = std::span{candidate.raw_payload}.subspan(
                    candidate_header.header_size, candidate_header.payload_bytes_0x20);
                const auto pcm = std::span{object.raw_payload}.subspan(header.header_size, header.payload_bytes_0x20);
                return std::ranges::equal(candidate_pcm, pcm);
            });
            if (duplicate == unique_indices.end())
                return false;
            continue;
        }
        if (header.payload_offset_0x24 != covered)
            return false;
        covered = *logical_end;
        unique_indices.push_back(index);
    }
    if (covered != first_header.payload_bytes_0x1c)
        return false;

    auto &assembled_object = objects[unique_indices.front()];
    const auto header_size = static_cast<std::size_t>(first_header.header_size);
    std::vector<std::byte> assembled{assembled_object.raw_payload.begin(),
                                     assembled_object.raw_payload.begin() + static_cast<std::ptrdiff_t>(header_size)};
    assembled.resize(header_size + first_header.payload_bytes_0x1c);
    for (const auto index : unique_indices) {
        const auto &segment = objects[index];
        const auto &header = segment.decoded.header;
        const auto pcm = std::span{segment.raw_payload}.subspan(header.header_size, header.payload_bytes_0x20);
        std::ranges::copy(pcm,
                          assembled.begin() + static_cast<std::ptrdiff_t>(header_size + header.payload_offset_0x24));
    }
    ByteWriter writer{assembled};
    if (auto written = writer.write_be32(0x20U, first_header.payload_bytes_0x1c); !written)
        return std::unexpected{written.error()};
    if (auto written = writer.write_be32(0x24U, 0U); !written)
        return std::unexpected{written.error()};
    auto decoded = detail::decode_media_object(assembled, assembled.size());
    if (!decoded)
        return std::unexpected{decoded.error()};
    assembled_object.size = assembled.size();
    assembled_object.decoded = std::move(decoded->object);
    assembled_object.raw_payload = std::move(assembled);
    assembled_object.decode_issue = std::move(decoded->issue);
    return true;
}
} // namespace axk::detail
