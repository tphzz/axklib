#include "axklib/media.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "axklib/bytes.hpp"
#include "axklib/utf8.hpp"
#include "media_internal.hpp"

namespace axk {
namespace {

std::vector<std::string> path_parts(std::string_view path) {
    std::vector<std::string> result;
    for (const auto part : std::views::split(path, '/'))
        result.emplace_back(part.begin(), part.end());
    return result;
}

Result<void> prepare_axk_object_directory_entries(std::vector<AxkObjectDirectoryEntry> &entries,
                                                  std::string_view source_name, const CancellationToken &cancellation) {
    if (entries.size() > AxkObjectDirectory::maximum_entries) {
        return std::unexpected{detail::media_error(ErrorCode::io_unsupported_size,
                                                   "AXK object directory exceeds the entry limit", source_name)};
    }
    std::ranges::sort(entries, [](const auto &left, const auto &right) {
        const auto left_name = detail::upper_ascii(left.name);
        const auto right_name = detail::upper_ascii(right.name);
        return left_name == right_name ? left.name < right.name : left_name < right_name;
    });

    std::string previous_name;
    std::map<std::string, std::size_t> entries_by_parent;
    std::uint64_t total_payload_bytes{};
    for (const auto &entry : entries) {
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        const auto parts = path_parts(entry.name);
        if (parts.empty() || parts.size() > AxkObjectDirectory::maximum_depth ||
            std::ranges::any_of(parts, detail::unsafe_component) || !entry.reader) {
            return std::unexpected{
                detail::media_error(ErrorCode::invalid_argument, "AXK object directory entry is invalid", source_name)};
        }
        const auto parent = parts.size() == 1U ? std::string{} : parts.front();
        if (++entries_by_parent[parent] > AxkObjectDirectory::maximum_leaf_entries) {
            return std::unexpected{detail::media_error(
                ErrorCode::io_unsupported_size, "AXK object directory leaf exceeds the entry limit", source_name)};
        }
        const auto folded_name = detail::upper_ascii(entry.name);
        if (!previous_name.empty() && folded_name == previous_name) {
            return std::unexpected{detail::media_error(ErrorCode::invalid_argument,
                                                       "AXK object directory contains case-insensitive duplicate names",
                                                       source_name)};
        }
        previous_name = folded_name;

        if (entry.reader->size() > AxkObjectDirectory::maximum_payload_bytes - total_payload_bytes ||
            entry.reader->size() > std::numeric_limits<std::size_t>::max()) {
            return std::unexpected{detail::media_error(ErrorCode::io_unsupported_size,
                                                       "AXK object directory exceeds the payload limit", source_name)};
        }
        total_payload_bytes += entry.reader->size();
    }
    return {};
}

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

struct ByteVectorLess {
    bool operator()(std::span<const std::byte> left, std::span<const std::byte> right) const noexcept {
        const auto common = std::min(left.size(), right.size());
        for (std::size_t index = 0U; index < common; ++index) {
            const auto left_byte = std::to_integer<std::uint8_t>(left[index]);
            const auto right_byte = std::to_integer<std::uint8_t>(right[index]);
            if (left_byte != right_byte)
                return left_byte < right_byte;
        }
        return left.size() < right.size();
    }
};

Result<bool> assemble_smpl_segment_group(std::vector<MediaObject> &objects, std::vector<std::size_t> &indices) {
    std::ranges::sort(indices, [&](std::size_t left, std::size_t right) {
        const auto &left_header = objects[left].decoded.header;
        const auto &right_header = objects[right].decoded.header;
        return std::tie(left_header.payload_offset_0x24, objects[left].logical_path) <
               std::tie(right_header.payload_offset_0x24, objects[right].logical_path);
    });
    const auto &first_header = objects[indices.front()].decoded.header;
    std::uint64_t covered{};
    std::vector<std::size_t> unique_indices;
    for (const auto index : indices) {
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

} // namespace

bool AxkObjectDirectory::recognizes_entry_prefix(std::span<const std::byte> prefix, bool nested) noexcept {
    if (!detail::object_prefix(prefix))
        return false;
    if (!nested)
        return true;
    constexpr std::size_t segment_header_size = 0x28U;
    if (prefix.size() < segment_header_size || detail::clean_ascii(prefix.subspan(0x0cU, 4U)) != "SMPL")
        return false;
    const auto be32 = [&prefix](std::size_t offset) {
        return (static_cast<std::uint32_t>(prefix[offset]) << 24U) |
               (static_cast<std::uint32_t>(prefix[offset + 1U]) << 16U) |
               (static_cast<std::uint32_t>(prefix[offset + 2U]) << 8U) |
               static_cast<std::uint32_t>(prefix[offset + 3U]);
    };
    return be32(0x24U) != 0U || be32(0x20U) != be32(0x1cU);
}

Result<bool> AxkObjectDirectory::recognizes(std::vector<AxkObjectDirectoryEntry> entries, std::string source_name,
                                            const CancellationToken &cancellation) {
    if (const auto prepared = prepare_axk_object_directory_entries(entries, source_name, cancellation); !prepared)
        return std::unexpected{prepared.error()};
    for (const auto &entry : entries) {
        const auto parts = path_parts(entry.name);
        const auto nested = parts.size() > 1U;
        constexpr std::size_t segment_header_size = 0x28U;
        const auto required_prefix_size = nested ? segment_header_size : detail::object_magic.size();
        const auto prefix_size =
            static_cast<std::size_t>(std::min<std::uint64_t>(entry.reader->size(), required_prefix_size));
        auto prefix = detail::read_bytes(*entry.reader, 0U, prefix_size, cancellation);
        if (!prefix)
            return std::unexpected{prefix.error()};
        if (recognizes_entry_prefix(*prefix, nested))
            return true;
    }
    return false;
}

Result<AxkObjectDirectory> AxkObjectDirectory::open(std::vector<AxkObjectDirectoryEntry> entries,
                                                    std::string source_name, const CancellationToken &cancellation) {
    if (const auto prepared = prepare_axk_object_directory_entries(entries, source_name, cancellation); !prepared)
        return std::unexpected{prepared.error()};

    AxkObjectDirectory result;
    result.source_name_ = std::move(source_name);
    bool nested{};
    for (auto &entry : entries) {
        nested = nested || path_parts(entry.name).size() > 1U;
        const auto prefix_size =
            static_cast<std::size_t>(std::min<std::uint64_t>(entry.reader->size(), detail::object_magic.size()));
        auto prefix = detail::read_bytes(*entry.reader, 0U, prefix_size, cancellation);
        if (!prefix)
            return std::unexpected{prefix.error()};
        if (!detail::object_prefix(*prefix))
            continue;
        auto bytes =
            detail::read_bytes(*entry.reader, 0U, static_cast<std::size_t>(entry.reader->size()), cancellation);
        if (!bytes)
            return std::unexpected{bytes.error()};
        auto decoded = detail::decode_media_object(*bytes, bytes->size());
        if (!decoded)
            return std::unexpected{decoded.error()};
        result.objects_.push_back({std::format("axk-object-directory:{}", entry.name),
                                   entry.name,
                                   "axk-object-directory",
                                   {},
                                   {},
                                   {"", LabelStatus::raw_identifier, "AXK object directory has no group label"},
                                   {"Object directory", LabelStatus::confirmed, "flat AXK object directory"},
                                   0U,
                                   bytes->size(),
                                   std::move(decoded->object),
                                   std::move(*bytes),
                                   std::move(decoded->issue)});
    }
    if (result.objects_.empty()) {
        return std::unexpected{detail::media_error(
            ErrorCode::container_unrecognized, "directory contains no recognized Yamaha objects", result.source_name_)};
    }
    std::map<std::vector<std::byte>, std::vector<std::size_t>, ByteVectorLess> segment_groups;
    for (std::size_t index = 0U; index < result.objects_.size(); ++index) {
        const auto &object = result.objects_[index];
        if (object.decoded.header.type != ObjectType::smpl ||
            (object.decoded.header.payload_offset_0x24 == 0U &&
             object.decoded.header.payload_bytes_0x20 == object.decoded.header.payload_bytes_0x1c)) {
            continue;
        }
        auto identity = smpl_segment_identity(object);
        if (!identity.empty())
            segment_groups[std::move(identity)].push_back(index);
    }
    if (nested && segment_groups.empty()) {
        return std::unexpected{
            detail::media_error(ErrorCode::container_unrecognized,
                                "nested directory contains no segmented Yamaha Wave Data; navigate to an object leaf",
                                result.source_name_)};
    }
    std::vector<bool> consumed(result.objects_.size());
    for (auto &[identity, indices] : segment_groups) {
        static_cast<void>(identity);
        if (indices.size() < 2U)
            continue;
        auto assembled = assemble_smpl_segment_group(result.objects_, indices);
        if (!assembled)
            return std::unexpected{assembled.error()};
        if (!*assembled)
            continue;
        for (std::size_t position = 1U; position < indices.size(); ++position)
            consumed[indices[position]] = true;
    }
    std::vector<MediaObject> assembled_objects;
    assembled_objects.reserve(result.objects_.size());
    for (std::size_t index = 0U; index < result.objects_.size(); ++index) {
        if (!consumed[index])
            assembled_objects.push_back(std::move(result.objects_[index]));
    }
    result.objects_ = std::move(assembled_objects);
    return result;
}

Result<AxkObjectDirectory> AxkObjectDirectory::open(const std::filesystem::path &path,
                                                    const CancellationToken &cancellation) {
    std::error_code error;
    const auto root_status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_directory(root_status) || std::filesystem::is_symlink(root_status)) {
        return std::unexpected{detail::media_error(
            ErrorCode::invalid_argument, "AXK object directory path is not a directory", text::path_to_utf8(path))};
    }
    std::vector<AxkObjectDirectoryEntry> entries;
    std::filesystem::recursive_directory_iterator iterator{path, std::filesystem::directory_options::none, error};
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
        return std::unexpected{detail::media_error(
            ErrorCode::io_read_failed, "could not enumerate AXK object directory", text::path_to_utf8(path))};
    }
    std::size_t visited_entries{};
    while (iterator != end) {
        if (++visited_entries > maximum_entries) {
            return std::unexpected{detail::media_error(ErrorCode::io_unsupported_size,
                                                       "AXK object directory exceeds the entry limit",
                                                       text::path_to_utf8(path))};
        }
        const auto entry_path = iterator->path();
        const auto status = iterator->symlink_status(error);
        if (error) {
            return std::unexpected{detail::media_error(ErrorCode::io_read_failed,
                                                       "could not inspect AXK object directory entry",
                                                       text::path_to_utf8(entry_path))};
        }
        if (std::filesystem::is_symlink(status)) {
            return std::unexpected{detail::media_error(ErrorCode::invalid_argument,
                                                       "AXK object directories may not contain links",
                                                       text::path_to_utf8(entry_path))};
        }
        if (std::filesystem::is_directory(status)) {
            if (iterator.depth() >= 1) {
                return std::unexpected{detail::media_error(
                    ErrorCode::invalid_argument, "AXK object directories support only one nested folder level",
                    text::path_to_utf8(entry_path))};
            }
        } else if (std::filesystem::is_regular_file(status)) {
            auto reader = FileReader::open(entry_path);
            if (!reader)
                return std::unexpected{reader.error()};
            const auto relative = std::filesystem::relative(entry_path, path, error);
            if (error) {
                return std::unexpected{detail::media_error(ErrorCode::io_read_failed,
                                                           "could not resolve AXK object directory entry",
                                                           text::path_to_utf8(entry_path))};
            }
            entries.push_back({text::path_to_utf8(relative), std::move(*reader)});
        } else {
            return std::unexpected{detail::media_error(ErrorCode::invalid_argument,
                                                       "AXK object directories contain an unsupported entry",
                                                       text::path_to_utf8(entry_path))};
        }
        iterator.increment(error);
        if (error) {
            return std::unexpected{detail::media_error(
                ErrorCode::io_read_failed, "could not enumerate AXK object directory", text::path_to_utf8(path))};
        }
    }
    return open(std::move(entries), text::path_to_utf8(path), cancellation);
}

const std::string &AxkObjectDirectory::source_name() const noexcept { return source_name_; }

const std::vector<MediaObject> &AxkObjectDirectory::stored_objects() const noexcept { return objects_; }

Result<std::vector<MediaObject>> AxkObjectDirectory::objects(MediaObjectReadMode mode,
                                                             const CancellationToken &cancellation) const {
    if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
    auto result = objects_;
    if (mode == MediaObjectReadMode::decoded_metadata) {
        for (auto &object : result) {
            if (object.decoded.header.type == ObjectType::smpl)
                object.raw_payload.clear();
        }
    }
    return result;
}

} // namespace axk
