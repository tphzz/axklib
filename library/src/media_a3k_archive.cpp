#include "axklib/media.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "axklib/utf8.hpp"
#include "media_internal.hpp"

namespace axk {
namespace {

constexpr std::size_t archive_header_size = 1110U;
constexpr std::size_t index_record_size = 271U;
constexpr std::size_t index_path_size = 256U;
constexpr std::size_t metadata_prefix_size = 0xacU;
constexpr std::string_view archive_magic = "A3k"
                                           "Dis"
                                           "kyPC";
constexpr std::string_view archive_marker = "XXXXXXXXXXXXXXXX";
constexpr std::string_view banner_path = "/A3kFileInfo.txt";

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

bool printable_path(std::span<const std::byte> bytes) {
    return std::ranges::all_of(bytes, [](std::byte value) {
        const auto ch = std::to_integer<unsigned char>(value);
        return ch == 0U || (ch >= 0x20U && ch <= 0x7eU);
    });
}

std::string source_stem(std::string_view source_name) {
    const auto path = text::path_from_utf8(source_name);
    if (path) {
        auto stem = text::path_to_utf8(path->stem());
        if (!stem.empty())
            return stem;
    }
    return "A3K archive";
}

std::string banner_volume(std::string_view banner) {
    constexpr std::string_view field = "Volume Name";
    const auto field_at = banner.find(field);
    if (field_at == std::string_view::npos)
        return {};
    const auto colon = banner.find(':', field_at + field.size());
    if (colon == std::string_view::npos)
        return {};
    const auto end = banner.find_first_of("\r\n", colon + 1U);
    return trim(std::string{banner.substr(colon + 1U, end == std::string_view::npos ? end : end - colon - 1U)});
}

struct IndexedIdentity {
    std::string volume;
    std::string type;
    std::string name;
    bool complete{};
};

IndexedIdentity indexed_identity(std::string_view path) {
    const auto first = path.find('\\');
    const auto last = path.rfind('\\');
    if (first == std::string_view::npos || last == first)
        return {};
    IndexedIdentity result{trim(std::string{path.substr(0U, first)}),
                           trim(std::string{path.substr(first + 1U, last - first - 1U)}),
                           trim(std::string{path.substr(last + 1U)}), false};
    result.complete = !result.volume.empty() && !result.type.empty() && !result.name.empty();
    return result;
}

MediaValidationIssue issue(std::string code, std::string message, std::string sampler_path, std::string basis) {
    return {std::move(code), std::move(message), std::move(sampler_path), std::move(basis),
            "Compare the archive index with the embedded Yamaha object header"};
}

bool expected_tail(std::span<const std::byte> record) {
    constexpr std::array tail{std::byte{1U}, std::byte{}, std::byte{}, std::byte{}, std::byte{}};
    return std::ranges::equal(record.subspan(266U, tail.size()), tail);
}

} // namespace

Result<A3kArchive> A3kArchive::open(std::shared_ptr<const RandomAccessReader> reader, std::string source_name,
                                    const CancellationToken &cancellation) {
    if (!reader)
        return std::unexpected{detail::media_error(ErrorCode::invalid_argument, "A3K archive reader is null")};
    if (reader->size() < archive_header_size || reader->size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected{detail::media_error(
            ErrorCode::io_unsupported_size, "A3K archive size is outside the supported 32-bit profile", source_name)};
    }
    auto header = detail::read_bytes(*reader, 0U, archive_header_size, cancellation);
    if (!header)
        return std::unexpected{header.error()};
    if (detail::clean_ascii(std::span{*header}.subspan(12U, archive_magic.size())) != archive_magic) {
        return std::unexpected{
            detail::media_error(ErrorCode::container_unrecognized, "file is not an A3K archive", source_name)};
    }
    if (detail::le32(*header, 0U) != 1U) {
        return std::unexpected{
            detail::media_error(ErrorCode::unsupported_profile, "A3K archive version is unsupported", source_name)};
    }
    if (detail::clean_ascii(std::span{*header}.subspan(70U, archive_marker.size())) != archive_marker) {
        return std::unexpected{detail::media_error(ErrorCode::container_invalid_geometry,
                                                   "A3K archive marker is invalid", source_name, 70U)};
    }
    const auto index_offset = static_cast<std::uint64_t>(detail::le32(*header, 4U));
    const auto count = static_cast<std::size_t>(detail::le32(*header, 8U));
    if (count == 0U || count > maximum_entries) {
        return std::unexpected{detail::media_error(ErrorCode::container_invalid_geometry,
                                                   "A3K index entry count exceeds configured bounds", source_name, 8U)};
    }
    const auto index_size = count * index_record_size;
    if (index_offset < archive_header_size || index_offset > reader->size() ||
        index_size != reader->size() - index_offset) {
        return std::unexpected{detail::media_error(ErrorCode::container_invalid_geometry,
                                                   "A3K index geometry does not match the archive size", source_name,
                                                   4U)};
    }
    auto index = detail::read_bytes(*reader, index_offset, index_size, cancellation);
    if (!index)
        return std::unexpected{index.error()};

    std::vector<A3kArchiveEntry> all_entries;
    all_entries.reserve(count);
    std::vector<MediaValidationIssue> issues;
    for (std::size_t ordinal = 0U; ordinal < count; ++ordinal) {
        const auto record = std::span{*index}.subspan(ordinal * index_record_size, index_record_size);
        const auto nul = std::ranges::find(record.first(index_path_size), std::byte{});
        const auto path_length = static_cast<std::size_t>(std::ranges::distance(record.begin(), nul));
        const auto path_bytes = record.first(path_length);
        auto path = detail::clean_ascii(path_bytes);
        if (nul == record.begin() + static_cast<std::ptrdiff_t>(index_path_size) || !printable_path(path_bytes)) {
            issues.push_back(issue("a3k_index_path_invalid",
                                   std::format("Archive entry {} has an invalid index path", ordinal), path,
                                   "A3K 256-byte index path field"));
        }
        const auto banner = ordinal == 0U;
        const auto expected_marker = banner ? 0U : 1U;
        if (std::to_integer<std::uint8_t>(record[256U]) != expected_marker || record[257U] != std::byte{} ||
            !expected_tail(record)) {
            return std::unexpected{detail::media_error(ErrorCode::container_invalid_geometry,
                                                       std::format("A3K index record {} marker is invalid", ordinal),
                                                       source_name, index_offset + ordinal * index_record_size + 256U)};
        }
        const auto offset = detail::le32(record, 258U);
        const auto size = detail::le32(record, 262U);
        if (offset < archive_header_size || offset > index_offset || size > index_offset - offset) {
            return std::unexpected{detail::media_error(
                ErrorCode::allocation_invalid_extent,
                std::format("A3K entry {} overlaps or exceeds the index", ordinal), source_name, offset)};
        }
        all_entries.push_back({static_cast<std::uint32_t>(ordinal), std::move(path), offset, size});
    }
    const auto &banner_entry = all_entries.front();
    if (banner_entry.indexed_path != banner_path || banner_entry.offset != archive_header_size ||
        banner_entry.size > maximum_banner_bytes) {
        return std::unexpected{detail::media_error(ErrorCode::container_invalid_geometry,
                                                   "A3K banner index entry is invalid", source_name,
                                                   banner_entry.offset)};
    }
    auto by_offset = all_entries;
    std::ranges::sort(by_offset, {}, &A3kArchiveEntry::offset);
    for (std::size_t index_value = 1U; index_value < by_offset.size(); ++index_value) {
        const auto previous_end =
            static_cast<std::uint64_t>(by_offset[index_value - 1U].offset) + by_offset[index_value - 1U].size;
        if (previous_end > by_offset[index_value].offset) {
            return std::unexpected{detail::media_error(ErrorCode::allocation_cross_link,
                                                       "A3K entry payload ranges overlap", source_name,
                                                       by_offset[index_value].offset)};
        }
    }
    auto banner_bytes = detail::read_bytes(*reader, banner_entry.offset, banner_entry.size, cancellation);
    if (!banner_bytes)
        return std::unexpected{banner_bytes.error()};
    std::string banner_text;
    banner_text.reserve(banner_bytes->size());
    for (const auto value : *banner_bytes) {
        const auto ch = std::to_integer<unsigned char>(value);
        banner_text.push_back(ch == 0U || ch == '\r' || ch == '\n' || ch == '\t' || (ch >= 0x20U && ch <= 0x7eU)
                                  ? static_cast<char>(ch)
                                  : '?');
    }

    std::vector<A3kArchiveEntry> objects{std::next(all_entries.begin()), all_entries.end()};
    std::map<std::string, std::string> indexed_volumes;
    for (const auto &entry : objects) {
        if (entry.size < 0x42U) {
            return std::unexpected{detail::media_error(ErrorCode::container_truncated,
                                                       std::format("A3K object entry {} is truncated", entry.ordinal),
                                                       source_name, entry.offset)};
        }
        auto prefix = detail::read_bytes(*reader, entry.offset,
                                         std::min<std::uint32_t>(entry.size, metadata_prefix_size), cancellation);
        if (!prefix)
            return std::unexpected{prefix.error()};
        if (!detail::object_prefix(*prefix)) {
            return std::unexpected{detail::media_error(
                ErrorCode::object_malformed, std::format("A3K entry {} is not a Yamaha object", entry.ordinal),
                source_name, entry.offset)};
        }
        auto decoded_header = decode_object_header(*prefix);
        if (!decoded_header)
            return std::unexpected{decoded_header.error()};
        const auto identity = indexed_identity(entry.indexed_path);
        if (!identity.complete) {
            issues.push_back(issue("a3k_index_path_incomplete",
                                   std::format("Archive entry {} has no complete placement path", entry.ordinal),
                                   entry.indexed_path, "A3K redundant index path"));
        } else {
            indexed_volumes.emplace(detail::upper_ascii(identity.volume), identity.volume);
            if (detail::upper_ascii(identity.type) != detail::upper_ascii(decoded_header->raw_type) ||
                identity.name != decoded_header->name) {
                issues.push_back(
                    issue("a3k_index_identity_mismatch",
                          std::format("Archive entry {} index identity differs from its object header", entry.ordinal),
                          entry.indexed_path, "Embedded FSFSDEV3SPLX type and name are authoritative"));
            }
        }
    }

    const auto banner_name = banner_volume(banner_text);
    MenuLabel label;
    if (indexed_volumes.size() == 1U) {
        label = {indexed_volumes.begin()->second, LabelStatus::confirmed, "A3K object index paths"};
    } else {
        if (indexed_volumes.size() > 1U) {
            issues.push_back(issue("a3k_index_volume_conflict", "Archive index contains conflicting volume names", {},
                                   "A3K archives represent one volume"));
        }
        if (!banner_name.empty())
            label = {banner_name, LabelStatus::navigation_aid, "A3K banner Volume Name"};
        else
            label = {source_stem(source_name), LabelStatus::navigation_aid, "archive source filename"};
    }

    A3kArchive result;
    result.reader_ = std::move(reader);
    result.source_name_ = std::move(source_name);
    result.banner_ = std::move(banner_text);
    result.volume_label_ = std::move(label);
    result.entries_ = std::move(objects);
    result.validation_issues_ = std::move(issues);
    return result;
}

Result<A3kArchive> A3kArchive::open(const std::filesystem::path &path, const CancellationToken &cancellation) {
    auto reader = FileReader::open(path);
    if (!reader)
        return std::unexpected{reader.error()};
    return open(std::move(*reader), text::path_to_utf8(path), cancellation);
}

const std::string &A3kArchive::source_name() const noexcept { return source_name_; }
const std::string &A3kArchive::banner() const noexcept { return banner_; }
const MenuLabel &A3kArchive::volume_label() const noexcept { return volume_label_; }
std::span<const A3kArchiveEntry> A3kArchive::entries() const noexcept { return entries_; }
std::span<const MediaValidationIssue> A3kArchive::validation_issues() const noexcept { return validation_issues_; }

Result<std::vector<std::byte>> A3kArchive::read_entry(const A3kArchiveEntry &entry,
                                                      const CancellationToken &cancellation) const {
    return read_entry_range(entry, 0U, entry.size, cancellation);
}

Result<std::vector<std::byte>> A3kArchive::read_entry_range(const A3kArchiveEntry &entry, std::uint64_t offset,
                                                            std::size_t size,
                                                            const CancellationToken &cancellation) const {
    const auto found = std::ranges::find(entries_, entry.ordinal, &A3kArchiveEntry::ordinal);
    if (found == entries_.end() || *found != entry) {
        return std::unexpected{
            detail::media_error(ErrorCode::object_missing, "A3K entry is not present in the archive", source_name_)};
    }
    if (offset > entry.size || size > entry.size - offset) {
        return std::unexpected{detail::media_error(ErrorCode::io_short_read, "A3K entry range is invalid", source_name_,
                                                   entry.offset + offset)};
    }
    return detail::read_bytes(*reader_, entry.offset + offset, size, cancellation);
}

Result<std::vector<std::byte>> A3kArchive::read_entry_prefix(const A3kArchiveEntry &entry, std::size_t maximum_bytes,
                                                             const CancellationToken &cancellation) const {
    return read_entry_range(entry, 0U, std::min<std::size_t>(entry.size, maximum_bytes), cancellation);
}

Result<std::vector<MediaObject>> A3kArchive::objects(std::size_t maximum_object_bytes,
                                                     const CancellationToken &cancellation) const {
    return objects(MediaObjectReadMode::complete, maximum_object_bytes, cancellation);
}

Result<std::vector<MediaObject>> A3kArchive::objects(MediaObjectReadMode mode, std::size_t maximum_object_bytes,
                                                     const CancellationToken &cancellation) const {
    std::vector<MediaObject> result;
    result.reserve(entries_.size());
    for (const auto &entry : entries_) {
        if (entry.size > maximum_object_bytes)
            continue;
        auto bytes = mode == MediaObjectReadMode::decoded_metadata
                         ? read_entry_prefix(entry, metadata_prefix_size, cancellation)
                         : read_entry(entry, cancellation);
        if (!bytes)
            return std::unexpected{bytes.error()};
        const bool wave_data = detail::clean_ascii(std::span{*bytes}.subspan(0x0cU, 4U)) == "SMPL";
        if (mode == MediaObjectReadMode::decoded_metadata && !wave_data) {
            bytes = read_entry(entry, cancellation);
            if (!bytes)
                return std::unexpected{bytes.error()};
        }
        auto decoded = detail::decode_media_object(*bytes, entry.size);
        if (!decoded)
            return std::unexpected{decoded.error()};
        auto raw_payload =
            mode == MediaObjectReadMode::decoded_metadata && wave_data ? std::vector<std::byte>{} : std::move(*bytes);
        const auto logical_path =
            std::format("{}/{}/{}", volume_label_.value, detail::object_category(decoded->object.header.type),
                        decoded->object.header.name);
        result.push_back({std::format("a3k:{}", entry.ordinal),
                          logical_path,
                          std::format("a3k:{}", source_name_),
                          {},
                          volume_label_.value,
                          {"", LabelStatus::raw_identifier, "A3K archive has no group"},
                          volume_label_,
                          entry.offset,
                          entry.size,
                          std::move(decoded->object),
                          std::move(raw_payload),
                          std::move(decoded->issue)});
    }
    return result;
}

} // namespace axk
