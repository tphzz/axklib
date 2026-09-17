#pragma once

#include "axklib/system_recording_parameters.hpp"

namespace axk::detail {

// Shared stored-field domains. Coupled editing rules are handled separately.
template <typename Visitor> void visit_system_recording_fields(bool native, const Visitor &visit) {
    using P = SystemRecordingParameters;
    visit(0x00U, &P::record_type, 0, native ? 2 : 3, "record type");
    visit(0x01U, &P::stereo, 0, 1, "stereo");
    visit(0x02U, &P::input, 0, 4, "input");
    visit(0x03U, &P::frequency_selection, 0, 6, "frequency selection");
    visit(0x04U, &P::pre_trigger_time, 0, 5, "pre-trigger time");
    visit(0x05U, &P::start_trigger, 0, 1, "start trigger");
    visit(0x06U, &P::stop_trigger, 0, 1, "stop trigger");
    visit(0x07U, &P::start_edge_level, 0, 63, "start edge level");
    visit(0x08U, &P::stop_edge_level, 0, 63, "stop edge level");
    visit(0x09U, &P::map_destination, 0, 2, "map destination");
    visit(0x0aU, &P::key_low, -1, 127, "low key");
    visit(0x0bU, &P::key_high, 0, 128, "high key");
    visit(0x0cU, &P::original_key, 0, 127, "original key");
    visit(0x0dU, &P::auto_normalize, 0, 1, "auto normalize");
    visit(0x0eU, &P::external_scsi_id, -1, 7, "external SCSI ID");
    visit(0x0fU, &P::external_track, 1, native ? 255 : 99, "external track");
    visit(0x10U, &P::external_index, 1, native ? 255 : 99, "external index");
    visit(0x11U, &P::monitor_output, 0, 5, "monitor output");
    visit(0x12U, &P::monitor_level, 0, 127, "monitor level");
    visit(0x13U, &P::click_level, 0, 127, "click level");
    visit(0x14U, &P::click_tempo_hundredths, 8000, 15999, "click tempo");
    visit(0x16U, &P::click_beat, 1, 15, "click beat");
    visit(0x17U, &P::monitor_enabled, 0, 1, "monitor enabled");
    visit(0x18U, &P::map_auto, 0, 1, "map auto");
    visit(0x19U, &P::map_original_key, 0, 127, "map original key");
    visit(0x1aU, &P::map_all_keys, 0, 1, "map all keys");
    if (!native)
        visit(0x33U, &P::ad_input_gain, 0, 1, "A/D input gain");
}

} // namespace axk::detail
