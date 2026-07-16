#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/application/contracts.hpp"
#include "axklib/application/operation_registry.hpp"

namespace axk::app {
class Sandbox;
class UploadStore;
} // namespace axk::app

namespace axk::cli {

// Adapts trusted CLI paths to the same sandbox references accepted by the
// application operations. The network server continues to use only its
// explicitly configured roots.
class LocalOperationRuntime {
  public:
    [[nodiscard]] static app::Result<std::unique_ptr<LocalOperationRuntime>>
    create(std::span<const std::filesystem::path> paths);

    ~LocalOperationRuntime();
    LocalOperationRuntime(const LocalOperationRuntime &) = delete;
    LocalOperationRuntime &operator=(const LocalOperationRuntime &) = delete;
    LocalOperationRuntime(LocalOperationRuntime &&) = delete;
    LocalOperationRuntime &operator=(LocalOperationRuntime &&) = delete;

    [[nodiscard]] app::Result<app::FileRef> file_ref(const std::filesystem::path &path) const;
    [[nodiscard]] app::Result<app::DirectoryRef> directory_ref(const std::filesystem::path &path) const;
    [[nodiscard]] app::FileRef scratch_file_ref(std::string filename) const;
    [[nodiscard]] app::Result<std::filesystem::path> resolve_file(const app::FileRef &reference) const;
    [[nodiscard]] app::Result<nlohmann::json> invoke(std::string_view operation_id, const nlohmann::json &input) const;

  private:
    struct RootMapping;

    LocalOperationRuntime(std::vector<RootMapping> roots, std::filesystem::path staging_directory,
                          std::map<std::filesystem::path, std::string> display_paths,
                          std::unique_ptr<app::Sandbox> sandbox, std::unique_ptr<app::UploadStore> uploads,
                          app::OperationRegistry registry);

    [[nodiscard]] app::Result<app::FileRef> reference(const std::filesystem::path &path) const;
    [[nodiscard]] std::string display_path(const app::FileRef &reference) const;

    std::vector<RootMapping> roots_;
    std::filesystem::path staging_directory_;
    std::map<std::filesystem::path, std::string> display_paths_;
    std::unique_ptr<app::Sandbox> sandbox_;
    std::unique_ptr<app::UploadStore> uploads_;
    app::OperationRegistry registry_;
};

int report_application_failure(const app::Error &error);

} // namespace axk::cli
