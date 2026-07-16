#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/application/filesystem.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/application/uploads.hpp"
#include "axklib/error.hpp"

namespace axk::app {

[[nodiscard]] Result<void> bind_manifest_operations(OperationRegistry &registry);
[[nodiscard]] Result<void> bind_write_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                 UploadStore &uploads);

struct LocalManifestInputBinding {
    std::string manifest_path;
    std::filesystem::path input_path;
};

struct PreparedLocalBuildManifest {
    nlohmann::json manifest;
    std::vector<LocalManifestInputBinding> bindings;
};

// Trusted local adapters use this to replace arbitrary local paths with the
// portable tokens required by the REST contract before invoking create.plan.
[[nodiscard]] axk::Result<PreparedLocalBuildManifest>
prepare_local_build_manifest(std::string_view kind, const std::filesystem::path &manifest_path);
[[nodiscard]] axk::Result<PreparedLocalBuildManifest>
prepare_local_alteration_manifest(const std::filesystem::path &manifest_path);

} // namespace axk::app
