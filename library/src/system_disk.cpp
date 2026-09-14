#include "axklib/system_file.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "axklib/bytes.hpp"

namespace axk {
namespace {

std::size_t disk_offset(SystemFileKind kind) {
    return current_record_envelope_size + (kind == SystemFileKind::a3000_system ? 0x60U : 0x1f0U);
}

Error invalid(const char *message) { return make_error(ErrorCode::invalid_argument, ErrorCategory::object, message); }

} // namespace

Result<DecodedSystemDisk> decode_system_disk(const DecodedSystemFile &file) {
    const auto encoded = encode_system_file(file);
    if (!encoded)
        return std::unexpected{encoded.error()};
    const bool native = file.kind == SystemFileKind::a3000_system;
    const auto bytes = std::span{*encoded}.subspan(disk_offset(file.kind), native ? 20U : 16U);
    const ByteReader reader{bytes};
    DecodedSystemDisk result;
    result.raw_bytes.assign(bytes.begin(), bytes.end());
    result.seed_accumulator = reader.be32(native ? 4U : 8U).value();
    auto &p = result.parameters;
    const auto id = reader.u8(0).value();
    if (id <= 7U)
        p.scsi_id = id;
    const auto top = reader.u8(native ? 8U : 12U).value();
    if (top <= 98U)
        p.top_partition = static_cast<std::uint8_t>(top + 1U);
    const std::uint32_t mask = native ? reader.u8(1).value() : reader.be32(4).value();
    for (std::size_t i = 0; i < p.scsi_mounts.size(); ++i)
        p.scsi_mounts[i] = (mask & (1U << i)) != 0U;
    if (!native)
        for (std::size_t i = 0; i < p.ide_mounts.size(); ++i)
            p.ide_mounts[i] = (mask & (1U << (8U + i))) != 0U;
    return result;
}

Result<DecodedSystemFile> patch_system_disk(const DecodedSystemFile &file, const SystemDiskParameters &patch,
                                            ASeriesModel model) {
    if (model != ASeriesModel::a3000 && model != ASeriesModel::a4000 && model != ASeriesModel::a5000)
        return std::unexpected{
            make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported, "Disk target model is unsupported")};
    const bool native = model == ASeriesModel::a3000;
    if (file.kind != (native ? SystemFileKind::a3000_system : SystemFileKind::a4000_a5000_system2))
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Disk target model does not match the System File")};
    auto encoded = encode_system_file(file);
    if (!encoded)
        return std::unexpected{encoded.error()};
    if (patch.scsi_id && *patch.scsi_id > 7U)
        return std::unexpected{invalid("Self ID must be 0..7")};
    if (patch.top_partition && (*patch.top_partition < 1U || *patch.top_partition > 98U))
        return std::unexpected{invalid("Top Partition must be 1..98; System Load resets 99 to 1")};
    const auto supplied = [](const auto &value) { return value.has_value(); };
    const bool ide_edit = std::ranges::any_of(patch.ide_mounts, supplied);
    if (native && ide_edit)
        return std::unexpected{invalid("IDE mount selections are unavailable on A3000")};
    const auto offset = disk_offset(file.kind);
    auto bytes = std::span{*encoded}.subspan(offset, native ? 20U : 16U);
    const ByteReader reader{bytes};
    const auto id = patch.scsi_id.value_or(static_cast<std::uint8_t>(reader.u8(0).value() & 7U));
    if (patch.scsi_mounts[id].value_or(false))
        return std::unexpected{invalid("The sampler cannot mount its own SCSI ID")};
    if (patch.scsi_id || ide_edit || std::ranges::any_of(patch.scsi_mounts, supplied)) {
        std::uint32_t mask = native ? reader.u8(1).value() : reader.be32(4).value();
        for (std::size_t i = 0; i < 10U; ++i) {
            const auto &value = i < 8U ? patch.scsi_mounts[i] : patch.ide_mounts[i - 8U];
            if (value)
                mask = *value ? mask | (1U << i) : mask & ~(1U << i);
        }
        // Saved mount choices must agree with the final load-effective Self ID.
        mask &= ~(1U << id);
        ByteWriter writer{bytes};
        const auto written = native ? writer.write_u8(1, static_cast<std::uint8_t>(mask)) : writer.write_be32(4, mask);
        if (!written)
            return std::unexpected{written.error()};
    }
    if (patch.scsi_id)
        bytes[0] = static_cast<std::byte>(*patch.scsi_id);
    if (patch.top_partition)
        bytes[native ? 8U : 12U] = static_cast<std::byte>(*patch.top_partition - 1U);
    return decode_system_file(file.kind, *encoded);
}

} // namespace axk
