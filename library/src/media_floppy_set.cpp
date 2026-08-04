#include "axklib/writer_internal.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>

#include "axklib/bytes.hpp"
#include "axklib/file_publication.hpp"
#include "axklib/media.hpp"
#include "axklib/package_archive.hpp"

namespace axk::detail {
namespace {

constexpr std::uint64_t floppy_image_bytes = 1'474'560U;
constexpr std::uint64_t floppy_data_clusters = 2'847U;
constexpr std::uint64_t cluster_bytes = 512U;
constexpr std::size_t floppy_root_entries = 224U;
constexpr std::size_t set_marker_entries = 1U;
constexpr std::size_t maximum_floppy_images = 32U;
constexpr std::string_view set_marker_name = "A3000F.SYM";

struct DiskState {
    FloppyDiskLayout layout;
    std::uint64_t used_clusters{};
    std::size_t used_entries{set_marker_entries};
};

Error floppy_error(std::string message) {
    return make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported, std::move(message));
}

Result<std::uint64_t> allocated_clusters(std::uint64_t bytes) {
    if (bytes > std::numeric_limits<std::uint64_t>::max() - (cluster_bytes - 1U))
        return std::unexpected{floppy_error("FAT12 allocation size overflowed")};
    return (bytes + cluster_bytes - 1U) / cluster_bytes;
}

std::string disk_name(std::string_view volume_name, std::size_t index) {
    std::string base;
    for (const auto character : volume_name) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) != 0) {
            base.push_back(static_cast<char>(std::toupper(byte)));
        } else if (character == ' ' || character == '_') {
            base.push_back(character);
        } else {
            base.push_back('_');
        }
        if (base.size() == 14U)
            break;
    }
    if (base.empty())
        base = "AXKLIB";
    base.resize(14U, ' ');
    return base + std::format("{:02}", index);
}

std::string disk_path(std::size_t index) { return std::format("payloads/disk{:02}.ima", index); }

Result<ObjectHeader> read_object_header(const PreparedMediaObject &object, const CancellationToken &cancellation) {
    if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
    if (object.payload == nullptr || object.size() < 0x42U)
        return std::unexpected{floppy_error("prepared floppy object has no readable current-object header")};
    std::vector<std::byte> prefix(0x42U);
    if (auto read = object.payload->read_exact_at(0U, prefix); !read)
        return std::unexpected{read.error()};
    return decode_object_header(prefix);
}

Result<void> validate_complete_smpl(const PreparedMediaObject &object, const ObjectHeader &header) {
    if (header.type != ObjectType::smpl || header.header_size < 0x42U || header.payload_offset_0x24 != 0U ||
        header.payload_bytes_0x20 != header.payload_bytes_0x1c ||
        static_cast<std::uint64_t>(header.header_size) + header.payload_bytes_0x1c != object.size()) {
        return std::unexpected{floppy_error(
            "multi-floppy export requires a complete Wave Data object with exact header and payload bounds")};
    }
    return {};
}

void add_whole_object(DiskState &disk, std::size_t object_index, std::uint64_t size, std::uint64_t clusters) {
    disk.layout.segments.push_back({object_index, 0U, size, 0U, false});
    disk.used_clusters += clusters;
    ++disk.used_entries;
}

nlohmann::ordered_json disk_set_manifest(const FloppyDiskSetPlan &plan, std::span<const std::string> digests) {
    nlohmann::ordered_json disks = nlohmann::ordered_json::array();
    for (std::size_t index = 0U; index < plan.disks.size(); ++index) {
        disks.push_back({{"index", index + 1U},
                         {"logicalName", plan.disks[index].name},
                         {"path", disk_path(index + 1U)},
                         {"sizeBytes", floppy_image_bytes},
                         {"sha256", digests[index]}});
    }
    return {{"schema", "axklib.floppy-disk-set.v1"},
            {"format", "YAMAHA_A_SERIES_MULTI_FLOPPY"},
            {"diskCount", plan.disks.size()},
            {"hardwareValidation", "PENDING"},
            {"setMarker", set_marker_name},
            {"yamahaSymbolMetadata", "NOT_SYNTHESIZED"},
            {"disks", std::move(disks)}};
}

std::vector<std::byte> json_bytes(const nlohmann::ordered_json &json) {
    auto text = json.dump(2);
    text.push_back('\n');
    const auto bytes = std::as_bytes(std::span{text});
    return {bytes.begin(), bytes.end()};
}

Result<std::uint64_t> projected_archive_bytes(const FloppyDiskSetPlan &plan) {
    std::vector<std::string> placeholders(plan.disks.size(), std::string(64U, '0'));
    const auto manifest = json_bytes(disk_set_manifest(plan, placeholders));
    std::uint64_t result = 22U;
    const auto account = [&](std::string_view path, std::uint64_t bytes) -> Result<void> {
        const auto overhead = 76U + 2U * path.size();
        if (bytes > std::numeric_limits<std::uint64_t>::max() - overhead ||
            result > std::numeric_limits<std::uint64_t>::max() - bytes - overhead) {
            return std::unexpected{floppy_error("multi-floppy archive size overflowed")};
        }
        result += bytes + overhead;
        return {};
    };
    if (auto added = account("manifest.json", manifest.size()); !added)
        return std::unexpected{added.error()};
    for (std::size_t index = 1U; index <= plan.disks.size(); ++index) {
        if (auto added = account(disk_path(index), floppy_image_bytes); !added)
            return std::unexpected{added.error()};
    }
    return result;
}

Result<std::vector<std::byte>> materialize(const PreparedMediaObject &object, const CancellationToken &cancellation) {
    if (object.payload == nullptr || object.size() > std::numeric_limits<std::size_t>::max())
        return std::unexpected{floppy_error("prepared floppy object is too large to materialize")};
    if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
    std::vector<std::byte> result(static_cast<std::size_t>(object.size()));
    if (auto read = object.payload->read_exact_at(0U, result); !read)
        return std::unexpected{read.error()};
    return result;
}

Result<std::vector<std::byte>> materialize_segment(const PreparedMediaObject &object,
                                                   const FloppyObjectSegment &segment,
                                                   const CancellationToken &cancellation) {
    if (!segment.split)
        return materialize(object, cancellation);
    if (segment.header_bytes > std::numeric_limits<std::size_t>::max() ||
        segment.payload_bytes > std::numeric_limits<std::size_t>::max() - segment.header_bytes ||
        segment.header_bytes > object.size() || segment.payload_offset > object.size() - segment.header_bytes ||
        segment.payload_bytes > object.size() - segment.header_bytes - segment.payload_offset) {
        return std::unexpected{floppy_error("prepared Wave Data continuation segment is out of bounds")};
    }
    std::vector<std::byte> result(static_cast<std::size_t>(segment.header_bytes + segment.payload_bytes));
    if (auto read = object.payload->read_exact_at(0U, std::span{result}.first(segment.header_bytes)); !read)
        return std::unexpected{read.error()};
    if (auto read = object.payload->read_exact_at(segment.header_bytes + segment.payload_offset,
                                                  std::span{result}.subspan(segment.header_bytes));
        !read) {
        return std::unexpected{read.error()};
    }
    ByteWriter writer{result};
    if (auto written = writer.write_be32(0x20U, static_cast<std::uint32_t>(segment.payload_bytes)); !written)
        return std::unexpected{written.error()};
    if (auto written = writer.write_be32(0x24U, static_cast<std::uint32_t>(segment.payload_offset)); !written)
        return std::unexpected{written.error()};
    return result;
}

Result<void> validate_disk(const PreparedMediaImage &prepared, std::span<const std::byte> bytes,
                           const CancellationToken &cancellation) {
    auto reader = std::make_shared<MemoryReader>(std::vector<std::byte>{bytes.begin(), bytes.end()});
    auto fat = FatImage::open(std::move(reader), "generated multi-floppy member", cancellation);
    if (!fat)
        return std::unexpected{fat.error()};
    auto objects = fat->objects(MediaObjectReadMode::complete,
                                static_cast<std::size_t>(prepared.limits.maximum_object_bytes), cancellation);
    if (!objects)
        return std::unexpected{objects.error()};
    using Identity = std::pair<std::uint64_t, std::string>;
    std::multiset<Identity> expected;
    std::multiset<Identity> actual;
    for (const auto &object : prepared.objects) {
        auto digest = package_internal::sha256_reader(*object.payload, cancellation);
        if (!digest)
            return std::unexpected{digest.error()};
        expected.emplace(object.size(), package_internal::hex_digest(*digest));
    }
    for (const auto &object : *objects)
        actual.emplace(object.raw_payload.size(),
                       package_internal::hex_digest(package_internal::sha256(object.raw_payload)));
    if (expected != actual)
        return std::unexpected{floppy_error("generated floppy member failed exact object reopen validation")};

    const auto root = fat->geometry().root_offset;
    const auto root_bytes = std::span{bytes}.subspan(static_cast<std::size_t>(root), floppy_root_entries * 32U);
    bool marker_found{};
    for (std::size_t offset = 0U; offset < root_bytes.size(); offset += 32U) {
        const auto entry = root_bytes.subspan(offset, 32U);
        if (entry[0] == std::byte{} || entry[0] == std::byte{0xe5})
            continue;
        const std::string stem(reinterpret_cast<const char *>(entry.data()), 8U);
        const std::string extension(reinterpret_cast<const char *>(entry.data() + 8U), 3U);
        marker_found = marker_found || (stem == "A3000F  " && extension == "SYM");
    }
    if (!marker_found)
        return std::unexpected{floppy_error("generated floppy member is missing A3000F.SYM")};
    return {};
}

Result<void> validate_reassembly(const PreparedMediaImage &source,
                                 const std::map<std::size_t, std::vector<std::vector<std::byte>>> &segments,
                                 const CancellationToken &cancellation) {
    for (const auto &[object_index, parts] : segments) {
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        auto original = materialize(source.objects[object_index], cancellation);
        if (!original)
            return std::unexpected{original.error()};
        auto header = decode_object_header(*original);
        if (!header)
            return std::unexpected{header.error()};
        std::vector<std::byte> assembled(original->begin(), original->begin() + header->header_size);
        assembled.resize(static_cast<std::size_t>(header->header_size) + header->payload_bytes_0x1c);
        std::uint64_t covered{};
        for (const auto &part : parts) {
            auto part_header = decode_object_header(part);
            if (!part_header)
                return std::unexpected{part_header.error()};
            if (covered > header->payload_bytes_0x1c || part_header->payload_offset_0x24 != covered ||
                part_header->payload_bytes_0x20 > header->payload_bytes_0x1c - covered) {
                return std::unexpected{floppy_error("generated Wave Data continuation ranges are not contiguous")};
            }
            const auto payload = std::span{part}.subspan(part_header->header_size, part_header->payload_bytes_0x20);
            std::ranges::copy(payload, assembled.begin() + static_cast<std::ptrdiff_t>(header->header_size + covered));
            covered += part_header->payload_bytes_0x20;
        }
        if (covered != header->payload_bytes_0x1c)
            return std::unexpected{floppy_error("generated Wave Data continuation set is incomplete")};
        ByteWriter writer{assembled};
        if (auto written = writer.write_be32(0x20U, header->payload_bytes_0x1c); !written)
            return std::unexpected{written.error()};
        if (auto written = writer.write_be32(0x24U, 0U); !written)
            return std::unexpected{written.error()};
        if (assembled != *original)
            return std::unexpected{floppy_error("generated Wave Data continuation set did not reassemble exactly")};
    }
    return {};
}

} // namespace

Result<FloppyDiskSetPlan> plan_floppy_disk_set(const PreparedMediaImage &image, std::string_view volume_name,
                                               const CancellationToken &cancellation) {
    if (image.objects.empty())
        return std::unexpected{floppy_error("a floppy disk set must contain at least one Yamaha object")};

    std::uint64_t single_clusters{};
    bool fits_single = image.objects.size() <= floppy_root_entries;
    for (const auto &object : image.objects) {
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        auto clusters = allocated_clusters(object.size());
        if (!clusters)
            return std::unexpected{clusters.error()};
        fits_single = fits_single && *clusters <= floppy_data_clusters - single_clusters;
        if (*clusters <= floppy_data_clusters - single_clusters)
            single_clusters += *clusters;
    }
    if (fits_single) {
        FloppyDiskSetPlan result;
        result.disks.push_back({disk_name(volume_name, 1U), {}});
        for (std::size_t index = 0U; index < image.objects.size(); ++index)
            result.disks.front().segments.push_back({index, 0U, image.objects[index].size(), 0U, false});
        result.projected_archive_bytes = floppy_image_bytes;
        return result;
    }

    std::vector<DiskState> disks;
    const auto add_disk = [&]() -> DiskState & {
        const auto index = disks.size() + 1U;
        disks.push_back({{disk_name(volume_name, index), {}}, 0U, set_marker_entries});
        return disks.back();
    };
    add_disk();
    for (std::size_t object_index = 0U; object_index < image.objects.size(); ++object_index) {
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        const auto &object = image.objects[object_index];
        auto whole_clusters = allocated_clusters(object.size());
        if (!whole_clusters)
            return std::unexpected{whole_clusters.error()};
        auto *disk = &disks.back();
        const auto fits_current = [&]() {
            return disk->used_entries < floppy_root_entries &&
                   *whole_clusters <= floppy_data_clusters - disk->used_clusters;
        };
        if (fits_current()) {
            add_whole_object(*disk, object_index, object.size(), *whole_clusters);
            continue;
        }
        if (*whole_clusters <= floppy_data_clusters) {
            disk = &add_disk();
            add_whole_object(*disk, object_index, object.size(), *whole_clusters);
            continue;
        }
        if (object.type != ObjectType::smpl) {
            return std::unexpected{floppy_error("only Wave Data objects may span multiple Yamaha floppy images")};
        }
        auto header = read_object_header(object, cancellation);
        if (!header)
            return std::unexpected{header.error()};
        if (auto valid = validate_complete_smpl(object, *header); !valid)
            return std::unexpected{valid.error()};
        std::uint64_t offset{};
        while (offset < header->payload_bytes_0x1c) {
            disk = &disks.back();
            const auto available_clusters = floppy_data_clusters - disk->used_clusters;
            const auto available_bytes = available_clusters * cluster_bytes;
            if (disk->used_entries >= floppy_root_entries || available_bytes <= header->header_size) {
                disk = &add_disk();
                continue;
            }
            const auto remaining = static_cast<std::uint64_t>(header->payload_bytes_0x1c) - offset;
            const auto local_bytes = std::min(remaining, available_bytes - header->header_size);
            if (local_bytes == 0U || local_bytes > std::numeric_limits<std::uint32_t>::max())
                return std::unexpected{floppy_error("Wave Data continuation segment size is unsupported")};
            auto segment_clusters = allocated_clusters(header->header_size + local_bytes);
            if (!segment_clusters)
                return std::unexpected{segment_clusters.error()};
            disk->layout.segments.push_back({object_index, offset, local_bytes, header->header_size, true});
            disk->used_clusters += *segment_clusters;
            ++disk->used_entries;
            offset += local_bytes;
        }
    }

    FloppyDiskSetPlan result;
    result.disks.reserve(disks.size());
    for (auto &disk : disks)
        result.disks.push_back(std::move(disk.layout));
    auto projection = projected_archive_bytes(result);
    if (!projection)
        return std::unexpected{projection.error()};
    result.projected_archive_bytes = *projection;
    return result;
}

Result<WrittenMediaImage> write_floppy_disk_set(const PreparedMediaImage &image, const FloppyDiskSetPlan &plan,
                                                const std::filesystem::path &output_path, bool overwrite,
                                                const CancellationToken &cancellation) {
    if (plan.disks.size() < 2U || plan.disks.size() > maximum_floppy_images)
        return std::unexpected{floppy_error("multi-floppy output requires between 2 and 32 images")};
    std::vector<package_internal::ArchiveEntry> entries;
    std::vector<std::string> digests;
    std::map<std::size_t, std::vector<std::vector<std::byte>>> split_segments;
    auto object_filenames = plan_fat12_object_filenames(image);
    if (!object_filenames)
        return std::unexpected{object_filenames.error()};
    entries.reserve(plan.disks.size() + 1U);
    digests.reserve(plan.disks.size());
    for (std::size_t disk_index = 0U; disk_index < plan.disks.size(); ++disk_index) {
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        PreparedMediaImage disk = image;
        disk.objects.clear();
        disk.iso_volumes.clear();
        disk.retained_files = {{std::string{set_marker_name}, {}}};
        for (const auto &segment : plan.disks[disk_index].segments) {
            auto bytes = materialize_segment(image.objects[segment.object_index], segment, cancellation);
            if (!bytes)
                return std::unexpected{bytes.error()};
            if (segment.split)
                split_segments[segment.object_index].push_back(*bytes);
            disk.objects.emplace_back(image.objects[segment.object_index].type,
                                      image.objects[segment.object_index].name, std::move(*bytes));
            disk.objects.back().fat_filename = (*object_filenames)[segment.object_index];
        }
        auto image_bytes = build_fat12_image(disk, cancellation);
        if (!image_bytes)
            return std::unexpected{image_bytes.error()};
        if (auto validated = validate_disk(disk, *image_bytes, cancellation); !validated)
            return std::unexpected{validated.error()};
        digests.push_back(package_internal::hex_digest(package_internal::sha256(*image_bytes)));
        entries.push_back({disk_path(disk_index + 1U), std::move(*image_bytes)});
    }
    if (auto validated = validate_reassembly(image, split_segments, cancellation); !validated)
        return std::unexpected{validated.error()};
    entries.push_back({"manifest.json", json_bytes(disk_set_manifest(plan, digests))});
    auto archive = package_internal::write_archive(std::move(entries));
    if (!archive)
        return std::unexpected{archive.error()};
    if (archive->size() != plan.projected_archive_bytes)
        return std::unexpected{floppy_error("multi-floppy archive size differs from its inspected projection")};
    if (archive->size() > image.limits.maximum_output_bytes)
        return std::unexpected{floppy_error("multi-floppy archive exceeds the configured output limit")};
    auto reopened = package_internal::read_archive(*archive);
    if (!reopened)
        return std::unexpected{reopened.error()};
    if (reopened->size() != plan.disks.size() + 1U || reopened->front().path != "manifest.json")
        return std::unexpected{floppy_error("multi-floppy archive failed deterministic reopen validation")};

    if (!overwrite && std::filesystem::exists(output_path))
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "fresh media output already exists")};
    std::error_code filesystem_error;
    if (!output_path.parent_path().empty())
        std::filesystem::create_directories(output_path.parent_path(), filesystem_error);
    if (filesystem_error)
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not create media output directory")};
    auto publication = TemporaryPublication::create(output_path);
    if (!publication)
        return std::unexpected{publication.error()};
    if (auto resized = publication->resize(archive->size()); !resized)
        return std::unexpected{resized.error()};
    if (auto written = publication->write_at(0U, *archive); !written)
        return std::unexpected{written.error()};
    if (auto flushed = publication->flush(); !flushed)
        return std::unexpected{flushed.error()};
    const auto mode = overwrite ? PublicationMode::replace_existing : PublicationMode::create_only;
    auto published = publication->publish(mode);
    if (!published)
        return std::unexpected{published.error()};
    return WrittenMediaImage{output_path,           MediaImageFormat::fat12_floppy,
                             archive->size(),       image.objects.size(),
                             std::move(*published), MediaConversionArtifactKind::floppy_disk_set,
                             plan.disks.size()};
}

} // namespace axk::detail
