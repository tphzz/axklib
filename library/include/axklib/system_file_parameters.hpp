#pragma once

#include <vector>

#include "axklib/system_favorites_parameters.hpp"
#include "axklib/system_global_parameters.hpp"
#include "axklib/system_panel_parameters.hpp"
#include "axklib/system_recording_parameters.hpp"

namespace axk {

// All requested groups are validated before publishing a retained System File.
// Empty groups leave their bytes unchanged; each group's edit dependencies apply.
struct SystemFilePatch {
    SystemGlobalParameters global;
    SystemRecordingPatch recording;
    std::vector<SystemEffectFavoritesPatch> favorites;
    SystemPanelPatch panel;
};

} // namespace axk
