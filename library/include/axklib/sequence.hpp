#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/export.hpp"
#include "axklib/object.hpp"

namespace axk {

AXK_API Result<CurrentSequence> decode_current_sequence(std::span<const std::byte> payload);

// Converts the native current SEQU timeline to Standard MIDI File
// format 0. The result contains one MTrk chunk and preserves native ticks.
AXK_API Result<std::vector<std::byte>> sequence_to_smf0(const CurrentSequence &sequence);

// Imports a bounded Standard MIDI File format 0 into the current SEQU profile.
// Input timing is normalized to the A-series 96 PPQN timeline.
AXK_API Result<std::vector<std::byte>> smf0_to_current_sequence(std::span<const std::byte> smf,
                                                                std::string_view sequence_name);

} // namespace axk
