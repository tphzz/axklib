#include "axklib/writer_internal.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <utility>

#include "axklib/bytes.hpp"
#include "axklib/media.hpp"
#include "axklib/package_archive.hpp"

namespace axk::detail {
namespace {

constexpr std::uint64_t floppy_image_bytes = 1'474'560U;
constexpr std::uint64_t floppy_data_clusters = 2'847U;
constexpr std::uint64_t cluster_bytes = 512U;
constexpr std::uint64_t yamaha_catalog_clusters = 20U;
constexpr std::size_t maximum_single_objects = 222U;
constexpr std::size_t maximum_member_segments = 221U;
constexpr std::size_t maximum_floppy_images = 32U;
constexpr std::string_view continuation_marker_name = "A3000F.SYM";
constexpr std::string_view final_marker_name = "A3000E.SYM";
constexpr auto carrier = "generated multi-floppy sets require a complete terminal Wave Data object at every rollover";

struct DiskState {
    FloppyDiskLayout layout;
    std::uint64_t used_clusters{yamaha_catalog_clusters};
};

struct WaveDataPlan {
    std::size_t object_index{};
    ObjectHeader header;
};

struct DependencyPlan {
    std::vector<std::size_t> objects;
    std::vector<std::optional<std::size_t>> anchor_sample_by_wave;
};

Error floppy_error(std::string message) {
    return make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported, std::move(message));
}

Result<std::uint64_t> allocated_clusters(std::uint64_t bytes) {
    if (bytes > std::numeric_limits<std::uint64_t>::max() - (cluster_bytes - 1U))
        return std::unexpected{floppy_error("FAT12 allocation size overflowed")};
    return (bytes + cluster_bytes - 1U) / cluster_bytes;
}

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
        header.payload_bytes_0x1c == 0U || header.payload_bytes_0x20 != header.payload_bytes_0x1c ||
        static_cast<std::uint64_t>(header.header_size) + header.payload_bytes_0x1c != object.size()) {
        return std::unexpected{floppy_error(
            "multi-floppy export requires a complete Wave Data object with exact header and payload bounds")};
    }
    return {};
}

Result<std::uint16_t> allocate_catalog_slot(const DiskState &disk) {
    const auto used = [&](std::uint16_t slot) {
        return std::ranges::any_of(disk.layout.segments,
                                   [slot](const auto &segment) { return segment.catalog_slot == slot; });
    };
    for (std::uint16_t slot = 2U; slot < 224U; ++slot) {
        if (!used(slot))
            return slot;
    }
    return std::unexpected{floppy_error("Yamaha floppy member has no free object-catalog slot")};
}

Result<void> add_whole_object(DiskState &disk, std::size_t object_index, std::uint64_t size, std::uint64_t clusters) {
    auto slot = allocate_catalog_slot(disk);
    if (!slot)
        return std::unexpected{slot.error()};
    disk.layout.segments.push_back({object_index, *slot, 0U, size, 0U, false, 0U});
    disk.used_clusters += clusters;
    return {};
}

Result<void> add_wave_segment(DiskState &disk, const WaveDataPlan &wave, std::uint64_t offset, std::uint64_t bytes,
                              std::uint16_t segment_ordinal) {
    auto clusters = allocated_clusters(static_cast<std::uint64_t>(wave.header.header_size) + bytes);
    if (!clusters)
        return std::unexpected{clusters.error()};
    if (bytes == 0U || disk.layout.segments.size() >= maximum_member_segments ||
        *clusters > floppy_data_clusters - disk.used_clusters) {
        return std::unexpected{floppy_error("Wave Data continuation segment does not fit its Yamaha floppy")};
    }
    auto slot = allocate_catalog_slot(disk);
    if (!slot)
        return std::unexpected{slot.error()};
    disk.layout.segments.push_back(
        {wave.object_index, *slot, offset, bytes, wave.header.header_size, true, segment_ordinal});
    disk.used_clusters += *clusters;
    return {};
}

bool continues_wave_data(const PreparedMediaImage &image, const FloppyDiskLayout &left, const FloppyDiskLayout &right) {
    if (left.segments.empty() || right.segments.empty())
        return false;
    const auto &left_segment = left.segments.back();
    const auto &right_segment = right.segments.front();
    return left_segment.split && right_segment.split && left_segment.object_index == right_segment.object_index &&
           left_segment.object_index < image.objects.size() &&
           image.objects[left_segment.object_index].type == ObjectType::smpl &&
           right_segment.payload_offset == left_segment.payload_offset + left_segment.payload_bytes &&
           right_segment.catalog_segment_ordinal == left_segment.catalog_segment_ordinal + 1U;
}

Result<void> validate_continuation_markers(const PreparedMediaImage &image, const FloppyDiskSetPlan &plan) {
    for (std::size_t index = 0U; index + 1U < plan.disks.size(); ++index) {
        if (plan.disks[index].marker_name != continuation_marker_name ||
            !continues_wave_data(image, plan.disks[index], plan.disks[index + 1U])) {
            return std::unexpected{floppy_error(
                "generated multi-floppy sets require a Wave Data continuation at every nonfinal boundary")};
        }
    }
    if (plan.disks.empty() || plan.disks.back().marker_name != final_marker_name)
        return std::unexpected{floppy_error("generated multi-floppy sets require a final A3000E.SYM marker")};
    return {};
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

    if (!prepared.floppy_catalog)
        return std::unexpected{floppy_error("generated floppy member has no expected Yamaha catalog")};
    const auto yamaha = std::ranges::find(fat->files(), std::string{"YAMAHA.SYM"}, &FatFile::path);
    if (yamaha == fat->files().end())
        return std::unexpected{floppy_error("generated floppy member is missing YAMAHA.SYM")};
    auto actual_catalog = fat->read_file(*yamaha, cancellation);
    if (!actual_catalog)
        return std::unexpected{actual_catalog.error()};
    auto expected_catalog = encode_yamaha_floppy_catalog(
        prepared.floppy_catalog->disk_name, prepared.floppy_catalog->files, prepared.floppy_catalog->categories);
    if (!expected_catalog)
        return std::unexpected{expected_catalog.error()};
    if (*actual_catalog != *expected_catalog)
        return std::unexpected{floppy_error("generated floppy member Yamaha catalog differs from its plan")};
    const auto marker = std::ranges::find_if(prepared.floppy_catalog->files, [](const auto &file) {
        return file.logical_path == "\\A3000.SYM" || file.logical_path == "\\A3000F.SYM" ||
               file.logical_path == "\\A3000E.SYM";
    });
    if (marker == prepared.floppy_catalog->files.end())
        return std::unexpected{floppy_error("generated floppy member catalog has no member marker")};
    auto marker_filename = yamaha_floppy_physical_filename(marker->logical_path.substr(1U), marker->slot);
    if (!marker_filename)
        return std::unexpected{marker_filename.error()};
    if (!std::ranges::contains(fat->files(), *marker_filename, &FatFile::path)) {
        std::string root_paths;
        for (const auto &file : fat->files())
            root_paths += (root_paths.empty() ? "" : ", ") + file.path;
        return std::unexpected{
            floppy_error(std::format("generated floppy member is missing physical member marker '{}' (root: {})",
                                     *marker_filename, root_paths))};
    }
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

Result<DependencyPlan> dependency_order(const PreparedMediaImage &image, const CancellationToken &cancellation) {
    std::vector<std::vector<std::size_t>> wave_data_by_sample(image.objects.size());
    std::vector<std::size_t> remaining_sample_uses(image.objects.size());
    for (const auto &dependency : image.sample_wave_dependencies) {
        if (dependency.sample_object_index >= image.objects.size() ||
            dependency.wave_data_object_index >= image.objects.size() ||
            image.objects[dependency.sample_object_index].type != ObjectType::sbnk ||
            image.objects[dependency.wave_data_object_index].type != ObjectType::smpl) {
            return std::unexpected{floppy_error("prepared Sample-to-Wave Data dependency is invalid")};
        }
        auto &targets = wave_data_by_sample[dependency.sample_object_index];
        if (!std::ranges::contains(targets, dependency.wave_data_object_index)) {
            targets.push_back(dependency.wave_data_object_index);
            ++remaining_sample_uses[dependency.wave_data_object_index];
        }
    }

    DependencyPlan result;
    result.anchor_sample_by_wave.resize(image.objects.size());
    std::vector<bool> emitted(image.objects.size());
    const auto emit = [&](std::size_t index) {
        if (!emitted[index]) {
            emitted[index] = true;
            result.objects.push_back(index);
        }
    };
    const auto emit_type = [&](ObjectType type) {
        for (std::size_t index = 0U; index < image.objects.size(); ++index) {
            if (image.objects[index].type == type)
                emit(index);
        }
    };

    emit_type(ObjectType::prog);
    for (std::size_t index = 0U; index < image.objects.size(); ++index) {
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        if (image.objects[index].type != ObjectType::sbnk)
            continue;
        emit(index);
        for (const auto wave_data : wave_data_by_sample[index]) {
            --remaining_sample_uses[wave_data];
            if (remaining_sample_uses[wave_data] != 0U)
                continue;
            if (!emitted[wave_data])
                result.anchor_sample_by_wave[wave_data] = index;
            emit(wave_data);
        }
    }
    emit_type(ObjectType::smpl);
    emit_type(ObjectType::sbac);
    emit_type(ObjectType::sequ);
    emit_type(ObjectType::prf3);
    for (std::size_t index = 0U; index < image.objects.size(); ++index)
        emit(index);
    return result;
}

} // namespace

Result<FloppyDiskSetPlan> plan_floppy_disk_set(const PreparedMediaImage &image, std::string_view volume_name,
                                               const CancellationToken &cancellation) {
    if (image.objects.empty())
        return std::unexpected{floppy_error("a floppy disk set must contain at least one Yamaha object")};
    auto object_order = dependency_order(image, cancellation);
    if (!object_order)
        return std::unexpected{object_order.error()};
    std::uint64_t single_clusters{yamaha_catalog_clusters};
    bool fits_single = image.objects.size() <= maximum_single_objects;
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
        auto name = yamaha_floppy_disk_name(volume_name, 1U);
        if (!name)
            return std::unexpected{name.error()};
        result.disks.push_back({std::move(*name), {}, 0U, {}});
        for (std::size_t index = 0U; index < object_order->objects.size(); ++index) {
            const auto object_index = object_order->objects[index];
            result.disks.front().segments.push_back({object_index, static_cast<std::uint16_t>(index + 2U), 0U,
                                                     image.objects[object_index].size(), 0U, false});
        }
        result.projected_archive_bytes = floppy_image_bytes;
        return result;
    }

    std::vector<DiskState> disks;
    const auto add_disk = [&]() -> Result<DiskState *> {
        const auto index = disks.size() + 1U;
        auto name = yamaha_floppy_disk_name(volume_name, index);
        if (!name)
            return std::unexpected{name.error()};
        disks.push_back({{std::move(*name), {}, 0U, {}}, yamaha_catalog_clusters});
        return &disks.back();
    };
    if (auto first_disk = add_disk(); !first_disk)
        return std::unexpected{first_disk.error()};
    const auto fits = [](const DiskState &disk, std::uint64_t clusters) {
        return disk.layout.segments.size() < maximum_member_segments &&
               clusters <= floppy_data_clusters - disk.used_clusters;
    };
    const auto chain_disk = [&]() -> Result<DiskState *> {
        auto &disk = disks.back();
        if (disk.layout.segments.empty() || disk.layout.segments.back().split)
            return std::unexpected{floppy_error(carrier)};
        auto &terminal = disk.layout.segments.back();
        const auto &object = image.objects[terminal.object_index];
        if (object.type != ObjectType::smpl)
            return std::unexpected{floppy_error(carrier)};
        auto header = read_object_header(object, cancellation);
        if (!header)
            return std::unexpected{header.error()};
        if (auto valid = validate_complete_smpl(object, *header); !valid)
            return std::unexpected{valid.error()};
        constexpr std::uint64_t tail_bytes = cluster_bytes;
        if (header->payload_bytes_0x1c <= tail_bytes)
            return std::unexpected{floppy_error(carrier)};
        auto whole_clusters = allocated_clusters(object.size());
        auto left_clusters = allocated_clusters(static_cast<std::uint64_t>(header->header_size) +
                                                header->payload_bytes_0x1c - tail_bytes);
        if (!whole_clusters || !left_clusters)
            return std::unexpected{!whole_clusters ? whole_clusters.error() : left_clusters.error()};
        terminal = {terminal.object_index,
                    terminal.catalog_slot,
                    0U,
                    header->payload_bytes_0x1c - tail_bytes,
                    header->header_size,
                    true,
                    1U};
        disk.used_clusters -= *whole_clusters - *left_clusters;
        const auto object_index = terminal.object_index;
        const auto left_payload_bytes = terminal.payload_bytes;
        auto next = add_disk();
        if (!next)
            return std::unexpected{next.error()};
        if (auto added = add_wave_segment(**next, {object_index, *header}, left_payload_bytes, tail_bytes, 2U);
            !added) {
            return std::unexpected{added.error()};
        }
        return *next;
    };

    constexpr auto empty_member_clusters = floppy_data_clusters - yamaha_catalog_clusters;
    for (std::size_t order_index = 0U; order_index < object_order->objects.size(); ++order_index) {
        const auto object_index = object_order->objects[order_index];
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        const auto &object = image.objects[object_index];
        auto whole_clusters = allocated_clusters(object.size());
        if (!whole_clusters)
            return std::unexpected{whole_clusters.error()};
        if (fits(disks.back(), *whole_clusters)) {
            if (auto added = add_whole_object(disks.back(), object_index, object.size(), *whole_clusters); !added)
                return std::unexpected{added.error()};
            continue;
        }
        if (object.type != ObjectType::smpl) {
            auto next = chain_disk();
            if (!next)
                return std::unexpected{next.error()};
            if (!fits(**next, *whole_clusters))
                return std::unexpected{floppy_error("prepared Yamaha object does not fit on an empty floppy")};
            if (auto added = add_whole_object(**next, object_index, object.size(), *whole_clusters); !added)
                return std::unexpected{added.error()};
            continue;
        }
        auto header = read_object_header(object, cancellation);
        if (!header)
            return std::unexpected{header.error()};
        if (auto valid = validate_complete_smpl(object, *header); !valid)
            return std::unexpected{valid.error()};
        const WaveDataPlan wave{object_index, std::move(*header)};
        const auto anchor_sample = object_order->anchor_sample_by_wave[object_index];
        const bool sample_on_current_member =
            anchor_sample &&
            std::ranges::contains(disks.back().layout.segments, *anchor_sample, &FloppyObjectSegment::object_index);
        const auto available_bytes = (floppy_data_clusters - disks.back().used_clusters) * cluster_bytes;
        if (sample_on_current_member && (disks.back().layout.segments.size() >= maximum_member_segments ||
                                         available_bytes <= wave.header.header_size)) {
            return std::unexpected{floppy_error(
                "a Sample and its first-use Wave Data cannot be separated without a continuation segment")};
        }
        if (*whole_clusters <= empty_member_clusters && !sample_on_current_member) {
            auto next = chain_disk();
            if (!next)
                return std::unexpected{next.error()};
            if (fits(**next, *whole_clusters)) {
                if (auto added = add_whole_object(**next, object_index, object.size(), *whole_clusters); !added)
                    return std::unexpected{added.error()};
                continue;
            }
        } else if (!sample_on_current_member && (disks.back().layout.segments.size() >= maximum_member_segments ||
                                                 available_bytes <= wave.header.header_size)) {
            if (auto next = chain_disk(); !next)
                return std::unexpected{next.error()};
        }
        std::uint64_t offset{};
        std::uint16_t segment_ordinal{1U};
        while (offset < wave.header.payload_bytes_0x1c) {
            auto *disk = &disks.back();
            const auto available_clusters = floppy_data_clusters - disk->used_clusters;
            const auto available_bytes = available_clusters * cluster_bytes;
            if (disk->layout.segments.size() >= maximum_member_segments || available_bytes <= wave.header.header_size) {
                auto next = add_disk();
                if (!next)
                    return std::unexpected{next.error()};
                continue;
            }
            const auto remaining = static_cast<std::uint64_t>(wave.header.payload_bytes_0x1c) - offset;
            const auto local_bytes = std::min(remaining, available_bytes - wave.header.header_size);
            if (local_bytes == 0U || local_bytes > std::numeric_limits<std::uint32_t>::max())
                return std::unexpected{floppy_error("Wave Data continuation segment size is unsupported")};
            if (auto added = add_wave_segment(*disk, wave, offset, local_bytes, segment_ordinal); !added)
                return std::unexpected{added.error()};
            offset += local_bytes;
            ++segment_ordinal;
            if (offset < wave.header.payload_bytes_0x1c) {
                auto next = add_disk();
                if (!next)
                    return std::unexpected{next.error()};
            }
        }
    }

    FloppyDiskSetPlan result;
    result.disks.reserve(disks.size());
    for (std::size_t index = 0U; index < disks.size(); ++index) {
        auto &disk = disks[index];
        std::set<std::uint16_t> used_slots;
        for (const auto &segment : disk.layout.segments)
            used_slots.insert(segment.catalog_slot);
        std::optional<std::uint16_t> marker_slot;
        for (std::uint16_t slot = 2U; slot < 224U; ++slot) {
            if (!used_slots.contains(slot)) {
                marker_slot = slot;
                break;
            }
        }
        if (!marker_slot)
            return std::unexpected{floppy_error("Yamaha floppy member has no free member-marker slot")};
        if (index + 1U < disks.size() && !continues_wave_data(image, disk.layout, disks[index + 1U].layout))
            return std::unexpected{floppy_error(
                "generated multi-floppy sets require a Wave Data continuation at every nonfinal boundary")};
        disk.layout.marker_name =
            std::string{index + 1U == disks.size() ? final_marker_name : continuation_marker_name};
        disk.layout.marker_slot = *marker_slot;
        result.disks.push_back(std::move(disk.layout));
    }
    auto projection = projected_floppy_archive_bytes(result);
    if (!projection)
        return std::unexpected{projection.error()};
    result.projected_archive_bytes = *projection;
    return result;
}

Result<std::vector<FloppyDiskMember>> build_floppy_disk_members(const PreparedMediaImage &image,
                                                                const FloppyDiskSetPlan &plan,
                                                                const CancellationToken &cancellation) {
    if (plan.disks.size() < 2U || plan.disks.size() > maximum_floppy_images)
        return std::unexpected{floppy_error("multi-floppy output requires between 2 and 32 images")};
    if (auto valid = validate_continuation_markers(image, plan); !valid)
        return std::unexpected{valid.error()};
    std::vector<FloppyDiskMember> members;
    std::map<std::size_t, std::vector<std::vector<std::byte>>> split_segments;
    members.reserve(plan.disks.size());
    for (std::size_t disk_index = 0U; disk_index < plan.disks.size(); ++disk_index) {
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        PreparedMediaImage disk = image;
        disk.objects.clear();
        disk.iso_volumes.clear();
        disk.floppy_catalog = YamahaFloppyCatalog{plan.disks[disk_index].name, {}, {}};
        const auto marker_filename =
            yamaha_floppy_physical_filename(plan.disks[disk_index].marker_name, plan.disks[disk_index].marker_slot);
        if (!marker_filename)
            return std::unexpected{marker_filename.error()};
        disk.retained_files = {{*marker_filename, {}}};
        for (const auto &segment : plan.disks[disk_index].segments) {
            auto bytes = materialize_segment(image.objects[segment.object_index], segment, cancellation);
            if (!bytes)
                return std::unexpected{bytes.error()};
            if (segment.split)
                split_segments[segment.object_index].push_back(*bytes);
            disk.objects.emplace_back(image.objects[segment.object_index].type,
                                      image.objects[segment.object_index].name, std::move(*bytes));
            auto physical_filename =
                yamaha_floppy_physical_filename(image.objects[segment.object_index].name, segment.catalog_slot);
            if (!physical_filename)
                return std::unexpected{physical_filename.error()};
            disk.objects.back().fat_filename = std::move(*physical_filename);
            auto logical_path = yamaha_floppy_object_path(
                image.objects[segment.object_index].type, image.objects[segment.object_index].name,
                segment.catalog_segment_ordinal == 0U ? std::nullopt
                                                      : std::optional<std::size_t>{segment.catalog_segment_ordinal});
            if (!logical_path)
                return std::unexpected{logical_path.error()};
            disk.floppy_catalog->files.push_back({segment.catalog_slot, std::move(*logical_path)});
        }
        disk.floppy_catalog->files.push_back(
            {plan.disks[disk_index].marker_slot, "\\" + plan.disks[disk_index].marker_name});
        disk.floppy_catalog->categories = yamaha_floppy_categories(disk.objects);
        auto catalog_bytes = encode_yamaha_floppy_catalog(disk.floppy_catalog->disk_name, disk.floppy_catalog->files,
                                                          disk.floppy_catalog->categories);
        if (!catalog_bytes)
            return std::unexpected{catalog_bytes.error()};
        auto catalog_digest = package_internal::hex_digest(package_internal::sha256(*catalog_bytes));
        auto image_bytes = build_fat12_image(disk, cancellation);
        if (!image_bytes)
            return std::unexpected{image_bytes.error()};
        if (auto validated = validate_disk(disk, *image_bytes, cancellation); !validated)
            return std::unexpected{validated.error()};
        auto digest = package_internal::hex_digest(package_internal::sha256(*image_bytes));
        members.push_back({std::move(*image_bytes), std::move(digest), std::move(catalog_digest)});
    }
    if (auto validated = validate_reassembly(image, split_segments, cancellation); !validated)
        return std::unexpected{validated.error()};
    return members;
}

} // namespace axk::detail
