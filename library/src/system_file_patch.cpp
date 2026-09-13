#include "axklib/system_file.hpp"

namespace axk {

Result<DecodedSystemFile> patch_system_file(const DecodedSystemFile &file, const SystemFilePatch &patch,
                                            ASeriesModel model) {
    const auto configured = patch_system_recording(file, patch.recording, model);
    if (!configured)
        return std::unexpected{configured.error()};
    const auto global = patch_system_global(*configured, patch.global, model);
    if (!global)
        return std::unexpected{global.error()};
    const auto favorites = patch_system_favorites(*global, patch.favorites, model);
    if (!favorites)
        return std::unexpected{favorites.error()};
    return patch_system_panel(*favorites, patch.panel, model);
}

} // namespace axk
