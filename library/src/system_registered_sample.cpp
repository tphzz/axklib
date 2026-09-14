#include "axklib/system_file.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>

#include "axklib/bytes.hpp"
#include "axklib/sample_parameter_codec.hpp"

namespace axk {
namespace {

Error rejected(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
}

Result<void> validate_template_dependencies(std::span<const std::byte> block, SampleParameters &patch,
                                            SampleParameterGeneration generation) {
    if (patch.root_key || patch.fine_tune_cents)
        return std::unexpected{rejected("Registered Sample pitch edits require an established attached-lane policy")};
    if (patch.expand_detune || patch.expand_dephase)
        return std::unexpected{rejected("Registered Sample expansion edits require the member topology")};

    // Validate only requested fields and their retained partners. Invalid
    // unrelated bytes must not prohibit an otherwise independent edit.
    SampleParameters effective = patch;
    const ByteReader reader{block};
    if (patch.loop_start_frame || patch.loop_length_frames) {
        const auto wave_start = *reader.be32(0x40);
        const auto wave_end = static_cast<std::uint64_t>(wave_start) + *reader.be32(0x48);
        if (wave_end > std::numeric_limits<std::uint32_t>::max() || wave_end != *reader.be32(0xb4))
            return std::unexpected{rejected("Registered Sample loop edits require a consistent retained wave window")};
        const auto start = patch.loop_start_frame.value_or(*reader.be32(0x50));
        const auto length = patch.loop_length_frames.value_or(*reader.be32(0x58));
        if (start < wave_start || static_cast<std::uint64_t>(start) + length > wave_end)
            return std::unexpected{rejected("Registered Sample loop interval exceeds the retained wave window")};
        // Explicit interval patches retain the unrequested partner, independently
        // of the panel's Length Lock gesture, and replace the coupled cache.
        patch.loop_start_frame = start;
        patch.loop_length_frames = length;
    }
    const auto pair = [&](auto &low, auto &high, const auto &requested_low, const auto &requested_high,
                          std::size_t low_offset, std::size_t high_offset) {
        if (requested_low || requested_high) {
            low = requested_low.value_or(*reader.u8(low_offset));
            high = requested_high.value_or(*reader.u8(high_offset));
        }
    };
    pair(effective.velocity_low, effective.velocity_high, patch.velocity_low, patch.velocity_high, 0x73, 0x72);
    pair(effective.filter_scaling_break1, effective.filter_scaling_break2, patch.filter_scaling_break1,
         patch.filter_scaling_break2, 0x64, 0x65);
    pair(effective.level_scaling_break1, effective.level_scaling_break2, patch.level_scaling_break1,
         patch.level_scaling_break2, 0x74, 0x75);
    if ((effective.filter_scaling_break1 && *effective.filter_scaling_break1 >= *effective.filter_scaling_break2) ||
        (effective.level_scaling_break1 && *effective.level_scaling_break1 >= *effective.level_scaling_break2))
        return std::unexpected{rejected("Registered Sample scaling breakpoints must be strictly increasing")};
    if (patch.key_low || patch.key_high) {
        const auto root = *reader.u8(0x2e);
        auto low = patch.key_low.value_or(*reader.u8(0x3b));
        auto high = patch.key_high.value_or(*reader.u8(0x3a));
        const bool special = low == sampler_original_key_low_limit || high == sampler_original_key_high_limit;
        if (special && root > 127U)
            return std::unexpected{rejected("Registered Sample special key limits require a valid retained root key")};
        // A special endpoint can move an unrequested opposite endpoint to root;
        // contradictory explicit endpoints are rejected as one atomic request.
        if (patch.key_high == sampler_original_key_high_limit && !patch.key_low && low <= 127U && low > root)
            patch.key_low = low = root;
        if (patch.key_low == sampler_original_key_low_limit && !patch.key_high && high <= 127U && high < root)
            patch.key_high = high = root;
        effective.key_low = low;
        effective.key_high = high;
        if (special)
            effective.root_key = root;
    }
    return detail::validate_sample_parameters(effective, generation);
}

} // namespace

Result<DecodedSystemFile> patch_system_registered_sample(const DecodedSystemFile &file, const SampleParameters &patch,
                                                         ASeriesModel model) {
    const bool native = model == ASeriesModel::a3000;
    if ((model != ASeriesModel::a3000 && model != ASeriesModel::a4000 && model != ASeriesModel::a5000) ||
        file.kind != (native ? SystemFileKind::a3000_system : SystemFileKind::a4000_a5000_system2))
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Registered Sample model does not match the System File")};
    if (const auto valid = encode_system_file(file); !valid)
        return std::unexpected{valid.error()};
    auto result = file;
    const auto block = std::span{result.system_bulk_bytes}.subspan(native ? 0x1c8U : 0x3ecU, native ? 0xbcU : 0xe0U);
    auto requested = patch;
    if (const auto valid = validate_template_dependencies(
            block, requested, native ? SampleParameterGeneration::a3000 : SampleParameterGeneration::current);
        !valid)
        return std::unexpected{valid.error()};
    // Dependency validation used the actual retained root. The scalar codec's
    // standalone default root is irrelevant to key-only template edits.
    auto key_low = requested.key_low;
    auto key_high = requested.key_high;
    requested.key_low.reset();
    requested.key_high.reset();
    if (const auto written = detail::apply_sample_parameters_to_block(
            block, requested, native ? detail::SampleParameterLayout::a3000 : detail::SampleParameterLayout::current);
        !written)
        return std::unexpected{written.error()};
    if (key_low)
        block[0x3bU] = static_cast<std::byte>(*key_low);
    if (key_high)
        block[0x3aU] = static_cast<std::byte>(*key_high);
    if (requested.loop_start_frame) {
        ByteWriter writer{block};
        if (auto written = writer.write_be32(0x54, *requested.loop_start_frame); !written)
            return std::unexpected{written.error()};
        if (auto written = writer.write_be32(0x5c, *requested.loop_length_frames); !written)
            return std::unexpected{written.error()};
        if (auto written = writer.write_be32(0xb8, *requested.loop_start_frame + *requested.loop_length_frames);
            !written)
            return std::unexpected{written.error()};
    }
    return result;
}

Result<DecodedSampleParameters> decode_system_registered_sample(const DecodedSystemFile &file) {
    if (file.kind != SystemFileKind::a3000_system && file.kind != SystemFileKind::a4000_a5000_system2)
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Registered Sample System File kind is unsupported")};
    const auto native = file.kind == SystemFileKind::a3000_system;
    if (file.storage_revision > (native ? 0U : 1U) || file.system_bulk_bytes.size() != (native ? 0x348U : 0xfe0U))
        return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                          "Registered Sample System File layout is invalid")};
    return decode_sample_parameter_block(
        std::span{file.system_bulk_bytes}.subspan(native ? 0x1c8U : 0x3ecU, native ? 0xbcU : 0xe0U),
        native ? SampleParameterGeneration::a3000 : SampleParameterGeneration::current);
}

} // namespace axk
