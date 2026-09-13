#include "axklib/system_file.hpp"

#include <array>
#include <cstddef>
#include <span>

#include "axklib/program_parameter_codec.hpp"

namespace axk {

Result<DecodedSystemFile> patch_system_recording(const DecodedSystemFile &file, const SystemRecordingPatch &patch,
                                                 ASeriesModel model) {
    const auto configured = patch_system_recording_configuration(file, patch.configuration, model);
    if (!configured)
        return std::unexpected{configured.error()};
    return patch_system_recording_effects(*configured, patch.effects, model);
}

Result<DecodedSystemFile> patch_system_recording_effects(const DecodedSystemFile &file,
                                                         const std::array<ProgramEffectParameters, 3> &patches,
                                                         ASeriesModel model) {
    const auto native = model == ASeriesModel::a3000;
    if ((model != ASeriesModel::a3000 && model != ASeriesModel::a4000 && model != ASeriesModel::a5000) ||
        file.kind != (native ? SystemFileKind::a3000_system : SystemFileKind::a4000_a5000_system2))
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Recording effect target model does not match the System File")};
    if (const auto valid = encode_system_file(file); !valid)
        return std::unexpected{valid.error()};
    auto result = file;
    auto effects = std::span{result.system_bulk_bytes}.subspan(native ? 0x110U : 0x320U, 120U);
    for (std::size_t slot = 0; slot < patches.size(); ++slot) {
        auto block = effects.subspan(slot * 40U).first<40>();
        if (const auto written = detail::apply_effect_parameter_block(block, patches[slot], model, slot,
                                                                      detail::EffectBlockKind::recording,
                                                                      detail::ProgramParameterWriteMode::patch);
            !written)
            return std::unexpected{written.error()};
    }
    return result;
}

} // namespace axk
