#include "axklib/sample_parameters.hpp"

#include <algorithm>
#include <bit>

#include "axklib/bytes.hpp"
#include "axklib/sample_parameter_rules.hpp"

namespace axk {

Result<DecodedSampleParameters> decode_sample_parameter_block(std::span<const std::byte> bytes,
                                                              SampleParameterGeneration generation) {
    if (generation != SampleParameterGeneration::a3000 && generation != SampleParameterGeneration::a4000_a5000)
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Sample parameter generation is unsupported")};
    const auto native = generation == SampleParameterGeneration::a3000;
    if (bytes.size() != (native ? 0xbcU : 0xe0U))
        return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                          "Sample parameter block has the wrong size for its generation")};
    const ByteReader r{bytes};
    DecodedSampleParameters result;
    result.generation = generation;
    result.raw_bytes.assign(bytes.begin(), bytes.end());
    result.sample_flags = *r.u8(0x28);
    result.mapout_flags = *r.u8(0x29);
    for (std::size_t index = 0; index < 4U; ++index)
        result.linked_program_bitmap_words[index] = *r.be32(0x18U + index * 4U);
    for (std::size_t lane = 0; lane < result.members.size(); ++lane) {
        result.members[lane] = {*r.u8(0x2eU + lane),        *r.be16(0x30U + 2U * lane), *r.s8(0x34U + lane),
                                *r.be16(0x36U + 2U * lane), *r.be32(0x40U + 4U * lane), *r.be32(0x48U + 4U * lane),
                                *r.be32(0x50U + 4U * lane), *r.be32(0x58U + 4U * lane)};
    }
    for (std::size_t index = 0; index < result.eq_coefficients.size(); ++index)
        result.eq_coefficients[index] = std::bit_cast<std::int16_t>(*r.be16(0xaaU + index * 2U));
    result.cached_wave_end = *r.be32(0xb4);
    result.cached_loop_end = *r.be32(0xb8);
    for (const auto &rule : sample_parameter_rules()) {
        if (const auto location = sample_parameter_location(rule, generation)) {
            const auto value = read_sample_parameter_value(bytes, *location);
            if (value && sample_parameter_value_allowed(*location, *value))
                rule.set(result.parameters, *value);
        }
    }
    if (!native)
        result.controller_copies_match = std::ranges::equal(bytes.first(0x18), bytes.subspan(0xbc, 0x18));
    return result;
}

} // namespace axk
