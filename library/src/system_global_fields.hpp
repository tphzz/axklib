#pragma once

#include <cstddef>

namespace axk::detail {

// Byte-aligned leaves shared by the reader and targeted writer. Offsets are
// global-relative; current-only leaves lie beyond the native 46-byte block.
template <typename Parameters, typename Visitor>
void visit_system_global_scalars(Parameters &p, bool port_b, Visitor visit) {
    visit(0, p.master_fine_tune, -63, 63, "master fine tune");
    visit(1, p.master_coarse_tune, -127, 127, "master coarse tune");
    visit(2, p.master_transpose, -127, 127, "master transpose");
    visit(3, p.velocity_curve_selection, 0, 17, "velocity curve");
    visit(4, p.basic_receive_channel_selection, 0, port_b ? 31 : 15, "Basic Receive Channel");
    visit(5, p.stereo_to_assignable_selection, 0, 5, "stereo-to-assignable output");
    for (std::size_t i = 0; i < 4U; ++i) {
        visit(7U + i, p.knob_transmit_channels[i], -1, port_b ? 32 : 16, "knob transmit channel");
        visit(0xbU + i, p.knob_control_devices[i], 0, 120, "knob control device");
    }
    for (std::size_t i = 0; i < 6U; ++i) {
        visit(0xfU + i, p.function_key_transmit_channels[i], 0, port_b ? 32 : 16, "function key transmit channel");
        visit(0x15U + i, p.function_key_notes[i], 0, 127, "function key note");
        visit(0x1bU + i, p.function_key_velocities[i], 1, 127, "function key velocity");
    }
    visit(0x21, p.stereo_output_level_offset, 0, 4, "stereo output level offset");
    visit(0x22, p.total_eq[0].frequency_selection, 4, 40, "low boost frequency");
    visit(0x23, p.total_eq[0].gain_selection, 52, 76, "low boost gain");
    for (std::size_t i = 0; i < 3U; ++i) {
        auto &band = p.total_eq[i + 1U];
        const auto offset = 0x25U + i * 3U;
        visit(offset, band.frequency_selection, i == 2U ? 28 : 4, i == 0U ? 40 : 58, "EQ frequency");
        visit(offset + 1U, band.gain_selection, 52, 76, "EQ gain");
        visit(offset + 2U, band.width_selection, 10, 120, "EQ width");
    }
    visit(0x2e, p.program_mode, 0, 1, "Program mode");
    for (std::size_t i = 0; i < 32U; ++i)
        visit(0x30U + i, p.part_program_numbers[i], 1, 128, "part Program number");
    visit(0x1b8, p.remix_zone_start, 0, 7, "Remix zone start");
    visit(0x1b9, p.remix_zone_end, 1, 8, "Remix zone end");
    for (std::size_t i = 0; i < 5U; ++i)
        visit(0x1baU + i, p.assignable_output_level_offsets[i], 0, 4, "assignable output level offset");
}

} // namespace axk::detail
