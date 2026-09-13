#include "axklib/system_file.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>

#include "axklib/bytes.hpp"
#include "axklib/program_parameter_codec.hpp"

namespace axk {

Result<ProgramParameters> decode_system_registered_program(const DecodedSystemFile &file) {
    const auto a3000 = file.kind == SystemFileKind::a3000_system;
    if (!a3000 && file.kind != SystemFileKind::a4000_a5000_system2) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Registered Program storage kind is unsupported")};
    }
    if (file.storage_revision > (a3000 ? 0U : 1U) || file.system_bulk_bytes.size() != (a3000 ? 0x348U : 0xfe0U)) {
        return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                          "System registered Program storage is invalid")};
    }
    // The retained bulk begins at body offset 0x20, not at the SFS envelope.
    const std::span<const std::byte> bulk{file.system_bulk_bytes};
    const auto generation =
        a3000 ? detail::ProgramParameterGeneration::a3000 : detail::ProgramParameterGeneration::current;
    const detail::ProgramParameterBlocks blocks =
        a3000
            ? detail::ProgramParameterBlocks{bulk.subspan<0x314U, 0x16U>(), bulk.subspan<0x2fcU, 0x10U>(), std::nullopt}
            : detail::ProgramParameterBlocks{bulk.subspan<0x60cU, 0x16U>(), bulk.subspan<0x5ccU, 0x10U>(),
                                             bulk.subspan<0x5dcU, 0x28U>()};
    auto result = detail::decode_program_parameter_blocks(blocks, generation);
    const auto count = a3000 ? 3U : result.effects.size();
    for (std::size_t slot = 0; slot < count; ++slot) {
        const auto offset = a3000       ? 0x284U + slot * 0x28U
                            : slot < 3U ? 0x4ccU + slot * 0x28U
                                        : 0x554U + (slot - 3U) * 0x28U;
        const auto raw = bulk.subspan(offset, 0x28U);
        const ByteReader reader{raw};
        ProgEffectBlock effect;
        std::ranges::copy(raw, effect.raw_bytes.begin());
        effect.type = *reader.u8(a3000 ? 7U : 6U);
        for (std::size_t parameter = 0; parameter < effect.parameter_values.size(); ++parameter)
            effect.parameter_values[parameter] = *reader.be16(8U + parameter * 2U);
        result.effects[slot] = detail::decode_program_effect_parameters(effect, slot, generation);
    }
    return result;
}

} // namespace axk
