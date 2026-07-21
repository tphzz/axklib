#pragma once

#include <filesystem>
#include <span>

#include "axklib/audio_export.hpp"

namespace axk::audio_internal {

[[nodiscard]] Result<std::filesystem::path>
resolve_export_destination(const std::filesystem::path &output_directory,
                           std::span<const std::filesystem::path> relative_parts);

[[nodiscard]] Result<void> validate_export_plan_paths(const ExportPlan &plan,
                                                      const std::filesystem::path &output_directory);

} // namespace axk::audio_internal
