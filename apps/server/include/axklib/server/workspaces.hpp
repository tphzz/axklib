#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "axklib/application/filesystem.hpp"

namespace axk::server {

enum class WorkspaceStatus : std::uint8_t { available, read_only, missing, not_directory, permission_denied };
enum class WorkspaceConfigurationState : std::uint8_t { ready, no_available_workspace, configuration_error };

struct WorkspaceDefinition {
    std::string id;
    std::string display_name;
    std::filesystem::path path;
    bool writable{true};
};

struct WorkspaceInfo {
    WorkspaceDefinition definition;
    WorkspaceStatus status{WorkspaceStatus::missing};
    bool effective_writable{};
    std::optional<std::string> issue;
};

struct WorkspaceSnapshot {
    std::uint64_t revision{};
    WorkspaceConfigurationState state{WorkspaceConfigurationState::no_available_workspace};
    std::vector<WorkspaceInfo> workspaces;
    std::optional<std::string> configuration_issue;
};

class WorkspaceStore {
  public:
    [[nodiscard]] static app::Result<WorkspaceStore> open(std::filesystem::path path);
    [[nodiscard]] static app::Result<std::filesystem::path> default_path();

    [[nodiscard]] const std::filesystem::path &path() const noexcept;
    [[nodiscard]] app::Sandbox sandbox() const;
    [[nodiscard]] WorkspaceSnapshot snapshot();
    [[nodiscard]] app::Result<WorkspaceInfo> add(std::string display_name, std::filesystem::path path, bool writable,
                                                 std::uint64_t expected_revision);
    [[nodiscard]] app::Result<WorkspaceInfo> update(std::string_view id, std::optional<std::string> display_name,
                                                    std::optional<std::filesystem::path> path,
                                                    std::optional<bool> writable, std::uint64_t expected_revision);
    [[nodiscard]] app::Result<void> remove(std::string_view id, std::uint64_t expected_revision);
    [[nodiscard]] app::Result<std::optional<std::filesystem::path>> archive_and_reset();

  private:
    struct State;
    explicit WorkspaceStore(std::shared_ptr<State> state) : state_(std::move(state)) {}

    std::shared_ptr<State> state_;
};

[[nodiscard]] std::string_view workspace_status_name(WorkspaceStatus status) noexcept;
[[nodiscard]] std::string_view workspace_configuration_state_name(WorkspaceConfigurationState state) noexcept;

} // namespace axk::server
