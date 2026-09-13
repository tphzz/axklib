#include "axklib/system_file.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace axk {
namespace {

template <typename T> std::optional<T> choice(std::byte byte, unsigned maximum) {
    const auto value = std::to_integer<unsigned>(byte);
    if (value > maximum)
        return std::nullopt;
    return static_cast<T>(value);
}

std::optional<bool> enabled_choice(std::byte byte) {
    if (const auto value = choice<std::uint8_t>(byte, 1U))
        return *value == 0U;
    return std::nullopt;
}

} // namespace

Result<DecodedSystemPanel> decode_system_panel(const DecodedSystemFile &file) {
    if (file.kind != SystemFileKind::a3000_system && file.kind != SystemFileKind::a4000_a5000_system2)
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Panel System File kind is unsupported")};
    const bool native = file.kind == SystemFileKind::a3000_system;
    if (file.storage_revision > (native ? 0U : 1U) || file.system_bulk_bytes.size() != (native ? 0x348U : 0xfe0U))
        return std::unexpected{
            make_error(ErrorCode::object_malformed, ErrorCategory::object, "Panel System File layout is invalid")};

    const auto bytes = std::span{file.system_bulk_bytes}.subspan(native ? 0xd0U : 0x2e0U, 64U);
    DecodedSystemPanel result;
    std::ranges::copy(bytes, result.raw_bytes.begin());
    auto &p = result.parameters;
    p.effect_edit_mode = choice<SystemEffectEditMode>(bytes[1], 1U);
    for (std::size_t i = 0; i < p.knob_control_types.size(); ++i)
        p.knob_control_types[i] = choice<SystemKnobControlType>(bytes[2U + i], 4U);
    p.assignable_key_function = choice<SystemAssignableKeyFunction>(bytes[6], 5U);
    p.audition_trigger_mode = choice<SystemAuditionTriggerMode>(bytes[7], 1U);
    p.knob_1_type = choice<SystemKnob1Type>(bytes[17], 1U);
    p.format_drive_id = choice<std::uint8_t>(bytes[0], native ? 7U : 9U);
    if (const auto format = choice<std::uint8_t>(bytes[11], native ? 2U : 5U))
        p.format_type = static_cast<SystemFormatType>(*format + (native ? 3U : 0U));
    p.function_selection = choice<SystemFunctionSelection>(bytes[8], 2U);
    p.page_selection = choice<SystemPageSelection>(bytes[9], 1U);
    p.note_display = choice<SystemNoteDisplay>(bytes[10], 1U);
    p.end_type = choice<SystemEndType>(bytes[15], native ? 4U : 3U);
    if (const auto scope = choice<std::uint8_t>(bytes[19], 1U)) {
        p.layer_selection_scope = *scope == 0U ? SystemLayerSelectionScope::all_pages
                                  : native     ? SystemLayerSelectionScope::selection_page
                                               : SystemLayerSelectionScope::tree_page;
    }
    if (native) {
        p.sample_name_order_selection = choice<std::uint8_t>(bytes[12], 2U);
        p.program_on_placement = choice<SystemProgramOnPlacement>(bytes[13], 1U);
        p.bank_member_visibility = choice<SystemBankMemberVisibility>(bytes[14], 1U);
        p.audition_name_view = enabled_choice(bytes[18]);
        p.midi_to_sample_name_view = enabled_choice(bytes[20]);
    } else {
        p.sample_sort = choice<SystemSampleSort>(bytes[22], 2U);
        p.tree_sort = choice<SystemStatusSort>(bytes[23], 2U);
        p.sample_bank_sort = choice<SystemStatusSort>(bytes[24], 2U);
        p.import_view = choice<SystemImportView>(bytes[16], 3U);
        p.cdr_scsi_id = choice<std::uint8_t>(bytes[25], 7U);
        p.cdr_write_speed = choice<SystemCdrWriteSpeed>(bytes[26], 4U);
    }
    return result;
}

Result<DecodedSystemFile> patch_system_panel(const DecodedSystemFile &file, const SystemPanelPatch &patch,
                                             ASeriesModel model) {
    const bool native = model == ASeriesModel::a3000;
    if ((model != ASeriesModel::a3000 && model != ASeriesModel::a4000 && model != ASeriesModel::a5000) ||
        file.kind != (native ? SystemFileKind::a3000_system : SystemFileKind::a4000_a5000_system2))
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Panel target model does not match the System File")};
    auto encoded = encode_system_file(file);
    if (!encoded)
        return std::unexpected{encoded.error()};
    auto bytes = std::span{*encoded}.subspan(current_record_envelope_size + (native ? 0xf0U : 0x300U), 64U);
    const auto set = [&](std::size_t index, const auto &selection, unsigned maximum) {
        if (!selection)
            return true;
        const auto value = static_cast<unsigned>(*selection);
        if (value > maximum)
            return false;
        bytes[index] = static_cast<std::byte>(value);
        return true;
    };
    if (!set(0, patch.format_drive_id, native ? 7U : 9U) || !set(15, patch.end_type, native ? 4U : 3U) ||
        !set(8, patch.function_selection, 2U) || !set(9, patch.page_selection, 1U) ||
        !set(10, patch.note_display, 1U) || !set(1, patch.effect_edit_mode, 1U) ||
        !set(6, patch.assignable_key_function, 5U) || !set(7, patch.audition_trigger_mode, 1U) ||
        !set(17, patch.knob_1_type, 1U))
        return std::unexpected{make_error(ErrorCode::invalid_argument, ErrorCategory::object,
                                          "Panel preference is unavailable on the target model")};
    for (std::size_t i = 0; i < patch.knob_control_types.size(); ++i) {
        if (!set(2U + i, patch.knob_control_types[i], 4U))
            return std::unexpected{
                make_error(ErrorCode::invalid_argument, ErrorCategory::object, "Knob control type is invalid")};
    }
    if (patch.format_type) {
        const auto value = static_cast<unsigned>(*patch.format_type);
        if (value > 5U || (native && value < 3U))
            return std::unexpected{make_error(ErrorCode::invalid_argument, ErrorCategory::object,
                                              "Format type is unavailable on the target model")};
        bytes[11] = static_cast<std::byte>(value - (native ? 3U : 0U));
    }
    if (patch.layer_selection_scope) {
        const auto scope = *patch.layer_selection_scope;
        const auto page = native ? SystemLayerSelectionScope::selection_page : SystemLayerSelectionScope::tree_page;
        if (scope != SystemLayerSelectionScope::all_pages && scope != page)
            return std::unexpected{make_error(ErrorCode::invalid_argument, ErrorCategory::object,
                                              "Layer selection scope is unavailable on the target model")};
        bytes[19] = scope == SystemLayerSelectionScope::all_pages ? std::byte{} : std::byte{1};
    }
    const bool valid_sort = native ? !patch.sample_sort && !patch.tree_sort && !patch.sample_bank_sort &&
                                         set(12, patch.sample_name_order_selection, 2U) &&
                                         set(13, patch.program_on_placement, 1U) &&
                                         set(14, patch.bank_member_visibility, 1U)
                                   : !patch.sample_name_order_selection && !patch.program_on_placement &&
                                         !patch.bank_member_visibility && set(22, patch.sample_sort, 2U) &&
                                         set(23, patch.tree_sort, 2U) && set(24, patch.sample_bank_sort, 2U);
    if (!valid_sort)
        return std::unexpected{make_error(ErrorCode::invalid_argument, ErrorCategory::object,
                                          "Sorting preference is unavailable on the target model")};
    if (!native && (patch.audition_name_view || patch.midi_to_sample_name_view))
        return std::unexpected{make_error(ErrorCode::invalid_argument, ErrorCategory::object,
                                          "NameView preferences are available only on A3000")};
    if (patch.audition_name_view)
        bytes[18] = *patch.audition_name_view ? std::byte{} : std::byte{1};
    if (patch.midi_to_sample_name_view)
        bytes[20] = *patch.midi_to_sample_name_view ? std::byte{} : std::byte{1};
    if (native && (patch.import_view || patch.cdr_scsi_id || patch.cdr_write_speed))
        return std::unexpected{make_error(ErrorCode::invalid_argument, ErrorCategory::object,
                                          "Import and CD-R preferences are available only on A4000/A5000")};
    if (!set(16, patch.import_view, 3U) || !set(25, patch.cdr_scsi_id, 7U) || !set(26, patch.cdr_write_speed, 4U))
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::object, "Import or CD-R preference is invalid")};
    return decode_system_file(file.kind, *encoded);
}

} // namespace axk
