#pragma once

#include <vector>

#include "axklib/program_parameters.hpp"
#include "axklib/sample_parameters.hpp"
#include "axklib/system_disk_parameters.hpp"
#include "axklib/system_favorites_parameters.hpp"
#include "axklib/system_global_parameters.hpp"
#include "axklib/system_midi_parameters.hpp"
#include "axklib/system_mlan_parameters.hpp"
#include "axklib/system_panel_parameters.hpp"
#include "axklib/system_playback_parameters.hpp"
#include "axklib/system_recording_parameters.hpp"
#include "axklib/system_remix_parameters.hpp"

namespace axk {

// All requested groups are validated before publishing a retained System File.
// Empty groups leave their bytes unchanged; each group's edit dependencies apply.
struct SystemFilePatch {
    SystemGlobalParameters global;
    SystemRecordingPatch recording;
    std::vector<SystemEffectFavoritesPatch> favorites;
    SystemPanelPatch panel;
    SystemMlanParameters mlan;
    SystemMidiParameters midi;
    SystemDiskParameters disk;
    SystemPlaybackParameters playback;
    ProgramParameters registered_program;
    SampleParameters registered_sample;
    std::vector<SystemRegisteredRemixPatch> registered_remix;
};

} // namespace axk
