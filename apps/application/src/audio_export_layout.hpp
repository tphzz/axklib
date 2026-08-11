#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "axklib/audio_export.hpp"
#include "content_id.hpp"

namespace axk::app {

struct AudioExportLayout {
    std::filesystem::path selection_root;
    bool preserve_volume_roots{};
    bool render_stereo{};
};

std::string safe_audio_export_path_name(std::string_view value, std::string_view fallback);

axk::Result<void> apply_audio_export_layout(ExportPlan &plan, const AudioExportLayout &layout,
                                            PooledPathAllocator &pooled_paths);

} // namespace axk::app
