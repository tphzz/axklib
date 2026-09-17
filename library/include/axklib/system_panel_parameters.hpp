#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace axk {

enum class SystemFunctionSelection : std::uint8_t { first, last, hold };
enum class SystemPageSelection : std::uint8_t { first, last };
enum class SystemNoteDisplay : std::uint8_t { name, number };
enum class SystemEndType : std::uint8_t { address, length, time, beat, graph };
enum class SystemLayerSelectionScope : std::uint8_t { all_pages, selection_page, tree_page };
enum class SystemProgramOnPlacement : std::uint8_t { top, mixed };
enum class SystemBankMemberVisibility : std::uint8_t { hide, show };
enum class SystemSampleSort : std::uint8_t { off, name, receive_channel_and_name };
enum class SystemStatusSort : std::uint8_t { off, name, status_and_name };
enum class SystemEffectEditMode : std::uint8_t { full, favorite };
enum class SystemKnobControlType : std::uint8_t { off, on, step_1, step_2, step_3 };
enum class SystemAssignableKeyFunction : std::uint8_t {
    knob_control,
    damp,
    controller_reset,
    function_key_play,
    knob_and_function_key,
    midi_to_sample
};
enum class SystemAuditionTriggerMode : std::uint8_t { normal, toggle };
enum class SystemKnob1Type : std::uint8_t { page, sample };
enum class SystemImportView : std::uint8_t { all, sample_bank, sample, sequence };
enum class SystemCdrWriteSpeed : std::uint8_t { x1, x2, x4, x6, x8 };

// A semantic projection, not the on-disk encoding shared by both generations.
enum class SystemFormatType : std::uint8_t { logical, physical, one_partition, floppy_quick, floppy_2hd, floppy_2dd };

// Read-only projections. Missing values mean invalid storage or an unavailable
// generation-specific interpretation. This is not a patch or initialization API.
struct SystemPanelParameters {
    std::optional<SystemEffectEditMode> effect_edit_mode;
    // Array positions 0..3 describe physical Knobs 2..5.
    std::array<std::optional<SystemKnobControlType>, 4> knob_control_types{};
    std::optional<SystemAssignableKeyFunction> assignable_key_function;
    std::optional<SystemAuditionTriggerMode> audition_trigger_mode;
    std::optional<SystemKnob1Type> knob_1_type;
    std::optional<std::uint8_t> format_drive_id;
    std::optional<SystemFormatType> format_type;
    std::optional<SystemFunctionSelection> function_selection;
    std::optional<SystemPageSelection> page_selection;
    std::optional<SystemNoteDisplay> note_display;
    // End-position display/edit coordinates; graph is available only on A3000.
    std::optional<SystemEndType> end_type;
    std::optional<SystemLayerSelectionScope> layer_selection_scope;

    // A3000 only. Name order: stored choice 0, forward 1, backward 2.
    // The display-font label for choice 0 is not translated here.
    std::optional<std::uint8_t> sample_name_order_selection;
    std::optional<SystemProgramOnPlacement> program_on_placement;
    std::optional<SystemBankMemberVisibility> bank_member_visibility;
    std::optional<bool> audition_name_view;
    std::optional<bool> midi_to_sample_name_view;

    // A4000/A5000 only. Sample receive-channel sorting differs from status sorting.
    std::optional<SystemSampleSort> sample_sort;
    std::optional<SystemStatusSort> tree_sort;
    std::optional<SystemStatusSort> sample_bank_sort;
    std::optional<SystemImportView> import_view;
    std::optional<std::uint8_t> cdr_scsi_id;
    std::optional<SystemCdrWriteSpeed> cdr_write_speed;

    friend bool operator==(const SystemPanelParameters &, const SystemPanelParameters &) = default;
};

struct DecodedSystemPanel {
    SystemPanelParameters parameters;
    // Includes unclassified fields and bytes a device may normalize on load.
    std::array<std::byte, 64> raw_bytes{};

    friend bool operator==(const DecodedSystemPanel &, const DecodedSystemPanel &) = default;
};

// Only preferences with established edit dependencies belong in this patch.
// An absent field preserves its stored byte, including invalid stored values.
struct SystemPanelPatch {
    std::optional<SystemEffectEditMode> effect_edit_mode;
    // Independently optional edits for Knobs 2..5, not the selected-knob cursor.
    std::array<std::optional<SystemKnobControlType>, 4> knob_control_types{};
    // Changes the saved binding without executing the assigned action.
    std::optional<SystemAssignableKeyFunction> assignable_key_function;
    std::optional<SystemAuditionTriggerMode> audition_trigger_mode;
    std::optional<SystemKnob1Type> knob_1_type;
    // Saved command selections only; changing these never formats a device.
    std::optional<std::uint8_t> format_drive_id;
    // A3000 accepts only floppy choices; A4000/A5000 accept all choices.
    std::optional<SystemFormatType> format_type;
    std::optional<SystemEndType> end_type;
    std::optional<SystemFunctionSelection> function_selection;
    std::optional<SystemPageSelection> page_selection;
    std::optional<SystemNoteDisplay> note_display;
    // selection_page is A3000-only; tree_page is A4000/A5000-only.
    std::optional<SystemLayerSelectionScope> layer_selection_scope;
    // A3000 only. Choice 0 skips name sorting, not separate grouping passes.
    std::optional<std::uint8_t> sample_name_order_selection;
    std::optional<SystemProgramOnPlacement> program_on_placement;
    std::optional<SystemBankMemberVisibility> bank_member_visibility;
    // True enables the name display; neither field enables/disables playback.
    std::optional<bool> audition_name_view;
    std::optional<bool> midi_to_sample_name_view;
    // A4000/A5000 only; values describe saved preferences, not working list modes.
    std::optional<SystemSampleSort> sample_sort;
    std::optional<SystemStatusSort> tree_sort;
    std::optional<SystemStatusSort> sample_bank_sort;
    // A4000/A5000 command preferences; these edits never execute a command.
    std::optional<SystemImportView> import_view;
    std::optional<std::uint8_t> cdr_scsi_id;
    std::optional<SystemCdrWriteSpeed> cdr_write_speed;
};

} // namespace axk
