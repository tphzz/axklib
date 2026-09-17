#include "media_floppy_assembly.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "axklib/bytes.hpp"
#include "media_internal.hpp"

namespace axk::detail {
namespace {
Error set_error(ErrorCode code, std::string message, std::string_view source = {}) {
    return media_error(code, std::move(message), source);
}
struct SmplIdentity {
    std::string catalog_series_path;
    std::string object_name;
    std::array<std::byte, 64> normalized_header{};

    auto operator<=>(const SmplIdentity &) const = default;
};

SmplIdentity smpl_identity(const FloppyPendingObject &object) {
    auto header = object.object.decoded.header.raw_prefix;
    std::fill(header.begin() + 0x20, header.begin() + 0x28, std::byte{});
    auto catalog_series_path = detail::upper_ascii(object.catalog_path);
    const auto &decoded = object.object.decoded.header;
    const bool continuation =
        decoded.payload_offset_0x24 != 0U || decoded.payload_bytes_0x20 != decoded.payload_bytes_0x1c;
    if (continuation && catalog_series_path.size() >= 2U &&
        std::ranges::all_of(std::string_view{catalog_series_path}.substr(catalog_series_path.size() - 2U),
                            [](char value) { return value >= '0' && value <= '9'; })) {
        catalog_series_path.resize(catalog_series_path.size() - 2U);
    }
    return {std::move(catalog_series_path), object.object.decoded.header.name, header};
}

Result<MediaObject> assemble_smpl(std::vector<FloppyPendingObject *> parts, FloppySetStatus status,
                                  std::size_t maximum_object_bytes) {
    std::ranges::sort(parts, [](const FloppyPendingObject *left, const FloppyPendingObject *right) {
        return std::tie(left->object.decoded.header.payload_offset_0x24, left->catalog_path) <
               std::tie(right->object.decoded.header.payload_offset_0x24, right->catalog_path);
    });
    const auto total = parts.front()->object.decoded.header.payload_bytes_0x1c;
    const auto header_size = parts.front()->object.decoded.header.header_size;
    if (total > maximum_object_bytes || header_size > maximum_object_bytes - total) {
        return std::unexpected{
            set_error(ErrorCode::io_unsupported_size, "assembled Wave Data exceeds the configured object limit")};
    }
    std::uint64_t covered{};
    std::vector<FloppyPendingObject *> unique;
    for (auto *part : parts) {
        const auto &header = part->object.decoded.header;
        if (header.header_size != header_size || header.payload_bytes_0x1c != total ||
            (!part->object.raw_payload.empty() &&
             static_cast<std::uint64_t>(header.header_size) + header.payload_bytes_0x20 >
                 part->object.raw_payload.size()) ||
            header.payload_offset_0x24 > total || header.payload_bytes_0x20 > total - header.payload_offset_0x24) {
            return std::unexpected{
                set_error(ErrorCode::container_invalid_geometry, "Wave Data continuation metadata is inconsistent")};
        }
        if (header.payload_offset_0x24 < covered) {
            const auto duplicate = std::ranges::find_if(unique, [&](const FloppyPendingObject *candidate) {
                const auto &candidate_header = candidate->object.decoded.header;
                if (candidate_header.payload_offset_0x24 != header.payload_offset_0x24 ||
                    candidate_header.payload_bytes_0x20 != header.payload_bytes_0x20) {
                    return false;
                }
                if (candidate->object.raw_payload.empty() && part->object.raw_payload.empty())
                    return true;
                const auto candidate_bytes = std::span{candidate->object.raw_payload}.subspan(
                    candidate_header.header_size, candidate_header.payload_bytes_0x20);
                const auto bytes =
                    std::span{part->object.raw_payload}.subspan(header.header_size, header.payload_bytes_0x20);
                return std::ranges::equal(candidate_bytes, bytes);
            });
            if (duplicate == unique.end())
                return std::unexpected{
                    set_error(ErrorCode::container_invalid_geometry, "Wave Data continuation ranges overlap")};
            continue;
        }
        if (header.payload_offset_0x24 != covered)
            return std::unexpected{
                set_error(ErrorCode::container_invalid_geometry, "Wave Data continuation ranges contain a gap")};
        covered += header.payload_bytes_0x20;
        unique.push_back(part);
    }
    if (covered != total) {
        if (status == FloppySetStatus::complete) {
            return std::unexpected{set_error(ErrorCode::container_truncated,
                                             "complete floppy set is missing Wave Data continuation bytes")};
        }
        auto object = parts.front()->object;
        object.key = std::format("fat12-floppy-set:SMPL:{}:{}", parts.front()->slot, object.decoded.header.name);
        return object;
    }

    auto object = unique.front()->object;
    if (object.raw_payload.empty()) {
        object.key = std::format("fat12-floppy-set:SMPL:{}:{}", unique.front()->slot, object.decoded.header.name);
        object.size = static_cast<std::uint64_t>(header_size) + total;
        return object;
    }
    std::vector<std::byte> assembled(object.raw_payload.begin(),
                                     object.raw_payload.begin() + static_cast<std::ptrdiff_t>(header_size));
    assembled.resize(static_cast<std::size_t>(header_size) + total);
    for (const auto *part : unique) {
        const auto &header = part->object.decoded.header;
        const auto bytes = std::span{part->object.raw_payload}.subspan(header.header_size, header.payload_bytes_0x20);
        std::ranges::copy(bytes,
                          assembled.begin() + static_cast<std::ptrdiff_t>(header_size + header.payload_offset_0x24));
    }
    ByteWriter writer{assembled};
    if (auto written = writer.write_be32(0x20U, total); !written)
        return std::unexpected{written.error()};
    if (auto written = writer.write_be32(0x24U, 0U); !written)
        return std::unexpected{written.error()};
    auto decoded = detail::decode_media_object(assembled, assembled.size());
    if (!decoded)
        return std::unexpected{decoded.error()};
    object.key = std::format("fat12-floppy-set:SMPL:{}:{}", unique.front()->slot, decoded->object.header.name);
    object.size = assembled.size();
    object.decoded = std::move(decoded->object);
    object.raw_payload = std::move(assembled);
    object.decode_issue = std::move(decoded->issue);
    return object;
}

} // namespace

Result<std::vector<MediaObject>> assemble_floppy_objects(std::vector<FloppyPendingObject> pending,
                                                         FloppySetStatus status, std::size_t maximum_object_bytes,
                                                         std::string_view source_name, MediaObjectReadMode mode) {
    std::vector<MediaObject> result;
    std::map<std::pair<ObjectType, std::string>, std::size_t> nonsmpl;
    std::map<SmplIdentity, std::vector<FloppyPendingObject *>> smpl;
    for (auto &item : pending) {
        if (item.object.decoded.header.type == ObjectType::smpl) {
            smpl[smpl_identity(item)].push_back(&item);
            continue;
        }
        const auto identity = std::pair{item.object.decoded.header.type, item.object.decoded.header.name};
        const auto found = nonsmpl.find(identity);
        if (found == nonsmpl.end()) {
            item.object.key =
                std::format("fat12-floppy-set:{}:{}", detail::object_category(identity.first), identity.second);
            nonsmpl.emplace(identity, result.size());
            result.push_back(std::move(item.object));
        } else if (result[found->second].raw_payload != item.object.raw_payload) {
            return std::unexpected{set_error(ErrorCode::container_backup_mismatch,
                                             std::format("floppy members contain conflicting {} object '{}'",
                                                         detail::object_category(identity.first), identity.second),
                                             source_name)};
        }
    }
    for (auto &[identity, parts] : smpl) {
        static_cast<void>(identity);
        auto object = assemble_smpl(std::move(parts), status, maximum_object_bytes);
        if (!object)
            return std::unexpected{object.error()};
        result.push_back(std::move(*object));
    }
    std::ranges::sort(result, [](const MediaObject &left, const MediaObject &right) {
        return std::tie(left.decoded.header.type, left.decoded.header.name, left.key) <
               std::tie(right.decoded.header.type, right.decoded.header.name, right.key);
    });
    if (mode == MediaObjectReadMode::decoded_metadata) {
        for (auto &object : result) {
            if (object.decoded.header.type == ObjectType::smpl)
                object.raw_payload.clear();
        }
    }
    return result;
}
} // namespace axk::detail
