#include "axklib/media.hpp"

#include <algorithm>
#include <deque>
#include <format>
#include <ranges>
#include <set>

#include "axklib/utf8.hpp"
#include "media_internal.hpp"

namespace axk {
namespace {

struct IsoDirectoryRecord {
    std::uint32_t extent{};
    std::uint32_t size{};
    std::uint8_t flags{};
    std::string name;
};

Result<IsoDirectoryRecord> parse_iso_record(std::span<const std::byte> record, std::string_view source,
                                            std::uint64_t offset) {
    if (record.size() < 34U || std::to_integer<std::uint8_t>(record[0]) != record.size()) {
        return std::unexpected{detail::media_error(ErrorCode::container_invalid_geometry,
                                                   "malformed ISO9660 directory record", source, offset)};
    }
    const auto name_length = std::to_integer<std::uint8_t>(record[32]);
    if (33U + name_length > record.size()) {
        return std::unexpected{detail::media_error(ErrorCode::container_truncated,
                                                   "truncated ISO9660 directory record name", source, offset)};
    }
    const auto extent = detail::le32(record, 2);
    const auto size = detail::le32(record, 10);
    if (detail::be32(record, 6) != extent || detail::be32(record, 14) != size ||
        detail::be16(record, 30) != detail::le16(record, 28)) {
        return std::unexpected{detail::media_error(ErrorCode::container_invalid_geometry,
                                                   "ISO9660 both-endian directory fields disagree", source, offset)};
    }
    std::string name;
    if (name_length == 1U && std::to_integer<std::uint8_t>(record[33]) == 0U)
        name = ".";
    else if (name_length == 1U && std::to_integer<std::uint8_t>(record[33]) == 1U)
        name = "..";
    else {
        name = detail::clean_ascii(record.subspan(33, name_length));
        if (const auto version = name.find(';'); version != std::string::npos)
            name.resize(version);
        while (!name.empty() && name.back() == '.')
            name.pop_back();
    }
    if (name != "." && name != ".." && detail::unsafe_component(name)) {
        return std::unexpected{detail::media_error(ErrorCode::container_invalid_geometry,
                                                   std::format("unsafe ISO9660 name '{}'", name), source, offset)};
    }
    if ((std::to_integer<std::uint8_t>(record[25]) & 0x80U) != 0U) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "multi-extent ISO9660 files are unsupported")};
    }
    return IsoDirectoryRecord{extent, size, std::to_integer<std::uint8_t>(record[25]), std::move(name)};
}

Result<void> validate_iso_extent(const RandomAccessReader &reader, std::uint32_t sector, std::uint32_t size,
                                 std::uint32_t volume_sectors, std::string_view source,
                                 bool require_physical_bytes = true) {
    const auto start = static_cast<std::uint64_t>(sector) * detail::iso_sector_size;
    if (sector >= volume_sectors ||
        (require_physical_bytes && (start > reader.size() || size > reader.size() - start)) ||
        (size != 0U && (static_cast<std::uint64_t>(size) + detail::iso_sector_size - 1U) / detail::iso_sector_size >
                           volume_sectors - sector)) {
        return std::unexpected{detail::media_error(
            ErrorCode::allocation_invalid_extent,
            std::format("ISO9660 extent sector {} size {} is outside the volume", sector, size), source, start)};
    }
    return {};
}

} // namespace

Result<IsoImage> IsoImage::open(std::shared_ptr<const RandomAccessReader> reader, std::string source_name,
                                const CancellationToken &cancellation) {
    if (!reader)
        return std::unexpected{detail::media_error(ErrorCode::invalid_argument, "ISO reader is null")};
    const auto pvd = detail::read_bytes(*reader, detail::iso_pvd_sector * detail::iso_sector_size,
                                        detail::iso_sector_size, cancellation);
    if (!pvd)
        return std::unexpected{pvd.error()};
    if (std::to_integer<std::uint8_t>((*pvd)[0]) != 1U ||
        detail::clean_ascii(std::span{*pvd}.subspan(1, 5)) != "CD001" ||
        std::to_integer<std::uint8_t>((*pvd)[6]) != 1U) {
        return std::unexpected{detail::media_error(ErrorCode::container_unrecognized,
                                                   "primary ISO9660 volume descriptor not found at sector 16",
                                                   source_name)};
    }
    const auto declared_volume_sectors = detail::le32(*pvd, 80);
    if (detail::be32(*pvd, 84) != declared_volume_sectors || detail::le16(*pvd, 128) != detail::iso_sector_size ||
        detail::be16(*pvd, 130) != detail::iso_sector_size || declared_volume_sectors == 0U) {
        return std::unexpected{detail::media_error(ErrorCode::container_invalid_geometry,
                                                   "invalid ISO9660 primary volume geometry", source_name)};
    }
    const auto physical_volume_sectors = reader->size() / detail::iso_sector_size;
    if (physical_volume_sectors <= detail::iso_pvd_sector) {
        return std::unexpected{detail::media_error(ErrorCode::container_truncated,
                                                   "ISO9660 image ends before its data volume", source_name)};
    }
    const bool declared_tail_is_missing = declared_volume_sectors > physical_volume_sectors;
    const auto root_length = std::to_integer<std::uint8_t>((*pvd)[156]);
    if (root_length < 34U || 156U + root_length > pvd->size()) {
        return std::unexpected{detail::media_error(ErrorCode::container_invalid_geometry,
                                                   "ISO9660 root directory record is malformed", source_name)};
    }
    const auto root = parse_iso_record(std::span{*pvd}.subspan(156, root_length), source_name,
                                       detail::iso_pvd_sector * detail::iso_sector_size + 156U);
    if (!root)
        return std::unexpected{root.error()};
    if (const auto valid = validate_iso_extent(*reader, root->extent, root->size, declared_volume_sectors, source_name);
        !valid) {
        return std::unexpected{valid.error()};
    }
    struct Pending {
        std::string path;
        std::uint32_t extent;
        std::uint32_t size;
    };
    std::deque<Pending> pending{{"", root->extent, root->size}};
    std::set<std::pair<std::uint32_t, std::uint32_t>> visited;
    std::set<std::string> all_paths;
    std::vector<IsoFile> files;
    while (!pending.empty()) {
        const auto directory = std::move(pending.front());
        pending.pop_front();
        if (!visited.emplace(directory.extent, directory.size).second) {
            return std::unexpected{detail::media_error(ErrorCode::allocation_cycle,
                                                       std::format("ISO9660 directory cycle at '{}'", directory.path),
                                                       source_name)};
        }
        const auto bytes =
            detail::read_bytes(*reader, static_cast<std::uint64_t>(directory.extent) * detail::iso_sector_size,
                               directory.size, cancellation);
        if (!bytes)
            return std::unexpected{bytes.error()};
        std::set<std::string> names;
        std::size_t offset{};
        while (offset < bytes->size()) {
            const auto length = std::to_integer<std::uint8_t>((*bytes)[offset]);
            if (length == 0U) {
                offset = ((offset / detail::iso_sector_size) + 1U) * detail::iso_sector_size;
                continue;
            }
            if (offset % detail::iso_sector_size + length > detail::iso_sector_size ||
                offset + length > bytes->size()) {
                return std::unexpected{detail::media_error(
                    ErrorCode::container_truncated, "ISO9660 directory record crosses its sector or extent",
                    source_name, static_cast<std::uint64_t>(directory.extent) * detail::iso_sector_size + offset)};
            }
            const auto record =
                parse_iso_record(std::span{*bytes}.subspan(offset, length), source_name,
                                 static_cast<std::uint64_t>(directory.extent) * detail::iso_sector_size + offset);
            if (!record)
                return std::unexpected{record.error()};
            offset += length;
            if (record->name == "." || record->name == "..")
                continue;
            if (!names.insert(detail::upper_ascii(record->name)).second) {
                return std::unexpected{detail::media_error(ErrorCode::container_invalid_geometry,
                                                           std::format("duplicate ISO9660 name '{}'", record->name),
                                                           source_name)};
            }
            const auto path =
                directory.path.empty() ? record->name : std::format("{}/{}", directory.path, record->name);
            if (!all_paths.insert(detail::upper_ascii(path)).second) {
                return std::unexpected{detail::media_error(ErrorCode::container_invalid_geometry,
                                                           std::format("duplicate ISO9660 path '{}'", path),
                                                           source_name)};
            }
            const bool is_directory = (record->flags & 0x02U) != 0U;
            if (const auto valid = validate_iso_extent(*reader, record->extent, record->size, declared_volume_sectors,
                                                       source_name, is_directory || !declared_tail_is_missing);
                !valid) {
                return std::unexpected{valid.error()};
            }
            files.push_back({path, record->extent, record->size, is_directory});
            if (is_directory)
                pending.push_back({path, record->extent, record->size});
        }
    }
    std::ranges::sort(files, {}, &IsoFile::path);
    IsoImage result;
    result.reader_ = std::move(reader);
    result.source_name_ = std::move(source_name);
    result.volume_id_ = detail::clean_ascii(std::span{*pvd}.subspan(40, 32));
    result.files_ = std::move(files);
    auto labels = detail::read_yamaha_iso_menu_labels(result, cancellation);
    if (!labels)
        return std::unexpected{labels.error()};
    result.group_labels_ = std::move(labels->groups);
    result.volume_labels_ = std::move(labels->volumes);
    result.validation_issues_ = std::move(labels->validation_issues);
    return result;
}

Result<IsoImage> IsoImage::open(const std::filesystem::path &path, const CancellationToken &cancellation) {
    auto reader = FileReader::open(path);
    if (!reader)
        return std::unexpected{reader.error()};
    return open(std::move(*reader), text::path_to_utf8(path), cancellation);
}

const std::string &IsoImage::volume_id() const noexcept { return volume_id_; }
const std::string &IsoImage::source_name() const noexcept { return source_name_; }
const std::vector<IsoFile> &IsoImage::files() const noexcept { return files_; }
std::span<const std::pair<std::string, std::string>> IsoImage::group_labels() const noexcept { return group_labels_; }
std::span<const std::pair<std::string, std::string>> IsoImage::volume_labels() const noexcept { return volume_labels_; }
std::span<const MediaValidationIssue> IsoImage::validation_issues() const noexcept { return validation_issues_; }

Result<std::vector<std::byte>> IsoImage::read_file(const IsoFile &file, const CancellationToken &cancellation) const {
    return read_file_prefix(file, file.size, cancellation);
}

Result<std::vector<std::byte>> IsoImage::read_file_prefix(const IsoFile &file, std::size_t maximum_bytes,
                                                          const CancellationToken &cancellation) const {
    return read_file_range(file, 0U, std::min<std::size_t>(file.size, maximum_bytes), cancellation);
}

Result<std::vector<std::byte>> IsoImage::read_file_range(const IsoFile &file, std::uint64_t offset, std::size_t size,
                                                         const CancellationToken &cancellation) const {
    if (file.is_directory || offset > file.size || size > file.size - offset) {
        return std::unexpected{detail::media_error(
            ErrorCode::out_of_bounds, std::format("ISO file range exceeds '{}'", file.path), source_name_)};
    }
    return detail::read_bytes(*reader_,
                              static_cast<std::uint64_t>(file.extent_sector) * detail::iso_sector_size + offset, size,
                              cancellation);
}

} // namespace axk
