#include "axklib/system_file.hpp"

namespace axk {

Result<DecodedSystemFile> patch_system_file(const DecodedSystemFile &file, const SystemFilePatch &patch,
                                            ASeriesModel model) {
    const auto configured = patch_system_recording(file, patch.recording, model);
    if (!configured)
        return std::unexpected{configured.error()};
    const auto remix = patch_system_registered_remix(*configured, patch.registered_remix, model);
    if (!remix)
        return std::unexpected{remix.error()};
    const auto global = patch_system_global(*remix, patch.global, model);
    if (!global)
        return std::unexpected{global.error()};
    const auto favorites = patch_system_favorites(*global, patch.favorites, model);
    if (!favorites)
        return std::unexpected{favorites.error()};
    const auto panel = patch_system_panel(*favorites, patch.panel, model);
    if (!panel)
        return std::unexpected{panel.error()};
    const auto mlan = patch_system_mlan(*panel, patch.mlan, model);
    if (!mlan)
        return std::unexpected{mlan.error()};
    const auto midi = patch_system_midi(*mlan, patch.midi, model);
    if (!midi)
        return std::unexpected{midi.error()};
    const auto disk = patch_system_disk(*midi, patch.disk, model);
    if (!disk)
        return std::unexpected{disk.error()};
    const auto playback = patch_system_playback(*disk, patch.playback, model);
    if (!playback)
        return std::unexpected{playback.error()};
    const auto program = patch_system_registered_program(*playback, patch.registered_program, model);
    if (!program)
        return std::unexpected{program.error()};
    return patch_system_registered_sample(*program, patch.registered_sample, model);
}

} // namespace axk
