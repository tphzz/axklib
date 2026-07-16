#include "axklib/server/workspaces.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <set>

#include <nlohmann/json.hpp>

#include "axklib/application/secure_random.hpp"
#include "axklib/utf8.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

using Json = nlohmann::json;

axk::app::Error workspace_error(std::string code, std::string message) { return {std::move(code), std::move(message)}; }

bool can_write(const std::filesystem::path &path) {
#if defined(_WIN32)
    return _waccess(path.c_str(), 2) == 0;
#else
    return ::access(path.c_str(), W_OK) == 0;
#endif
}

axk::server::WorkspaceInfo inspect(const axk::server::WorkspaceDefinition &definition) {
    axk::server::WorkspaceInfo result{.definition = definition,
                                      .status = axk::server::WorkspaceStatus::missing,
                                      .effective_writable = false,
                                      .issue = std::nullopt};
    std::error_code error;
    const auto status = std::filesystem::symlink_status(definition.path, error);
    if (error == std::errc::no_such_file_or_directory || !std::filesystem::exists(status)) {
        result.status = axk::server::WorkspaceStatus::missing;
        result.issue = "workspace directory does not exist";
        return result;
    }
    if (error) {
        result.status = axk::server::WorkspaceStatus::permission_denied;
        result.issue = "workspace path cannot be inspected";
        return result;
    }
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
        result.status = axk::server::WorkspaceStatus::not_directory;
        result.issue = "workspace path is not a directory";
        return result;
    }
    const auto canonical = std::filesystem::canonical(definition.path, error);
    if (error) {
        result.status = axk::server::WorkspaceStatus::permission_denied;
        result.issue = "workspace directory is inaccessible";
        return result;
    }
    result.definition.path = canonical;
    result.effective_writable = definition.writable && can_write(canonical);
    result.status = result.effective_writable || !definition.writable ? axk::server::WorkspaceStatus::available
                                                                      : axk::server::WorkspaceStatus::read_only;
    if (result.status == axk::server::WorkspaceStatus::read_only)
        result.issue = "workspace was requested as writable but is currently read-only";
    return result;
}

Json serialize(std::uint64_t revision, const std::vector<axk::server::WorkspaceDefinition> &workspaces) {
    Json values = Json::array();
    for (const auto &workspace : workspaces) {
        values.push_back({{"id", workspace.id},
                          {"displayName", workspace.display_name},
                          {"path", axk::text::path_to_utf8(workspace.path)},
                          {"writable", workspace.writable}});
    }
    return {{"schemaVersion", 1U}, {"revision", revision}, {"workspaces", std::move(values)}};
}

axk::app::Result<void> publish(const std::filesystem::path &path, std::uint64_t revision,
                               const std::vector<axk::server::WorkspaceDefinition> &workspaces) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return std::unexpected(
            workspace_error("workspace_store_write_failed", "workspace store directory cannot be created"));
    const auto temporary = path.parent_path() / (path.filename().string() + ".tmp");
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        if (!output)
            return std::unexpected(
                workspace_error("workspace_store_write_failed", "workspace store cannot be written"));
        output << serialize(revision, workspaces).dump(2) << '\n';
        output.flush();
        if (!output)
            return std::unexpected(
                workspace_error("workspace_store_write_failed", "workspace store write did not complete"));
    }
#if defined(_WIN32)
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, error);
        return std::unexpected(
            workspace_error("workspace_store_write_failed", "workspace store cannot be replaced atomically"));
    }
#else
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return std::unexpected(
            workspace_error("workspace_store_write_failed", "workspace store cannot be replaced atomically"));
    }
#endif
    return {};
}

} // namespace

struct axk::server::WorkspaceStore::State {
    std::mutex mutex;
    std::filesystem::path path;
    app::Sandbox sandbox;
    std::uint64_t revision{};
    std::vector<WorkspaceDefinition> definitions;
    std::optional<std::string> configuration_issue;

    explicit State(std::filesystem::path value, app::Sandbox sandbox_value)
        : path(std::move(value)), sandbox(std::move(sandbox_value)) {}
};

std::string_view axk::server::workspace_status_name(WorkspaceStatus status) noexcept {
    switch (status) {
    case WorkspaceStatus::available:
        return "AVAILABLE";
    case WorkspaceStatus::read_only:
        return "READ_ONLY";
    case WorkspaceStatus::missing:
        return "MISSING";
    case WorkspaceStatus::not_directory:
        return "NOT_DIRECTORY";
    case WorkspaceStatus::permission_denied:
        return "PERMISSION_DENIED";
    }
    return "PERMISSION_DENIED";
}

std::string_view axk::server::workspace_configuration_state_name(WorkspaceConfigurationState state) noexcept {
    switch (state) {
    case WorkspaceConfigurationState::ready:
        return "READY";
    case WorkspaceConfigurationState::no_available_workspace:
        return "NO_AVAILABLE_WORKSPACE";
    case WorkspaceConfigurationState::configuration_error:
        return "CONFIGURATION_ERROR";
    }
    return "CONFIGURATION_ERROR";
}

axk::app::Result<std::filesystem::path> axk::server::WorkspaceStore::default_path() {
#if defined(_WIN32)
    const auto *base = std::getenv("APPDATA");
    if (base == nullptr || *base == '\0')
        return std::unexpected(workspace_error("workspace_store_unavailable", "APPDATA is not available"));
    return std::filesystem::path{base} / "axkdeck" / "workspaces.json";
#elif defined(__APPLE__)
    const auto *home = std::getenv("HOME");
    if (home == nullptr || *home == '\0')
        return std::unexpected(workspace_error("workspace_store_unavailable", "HOME is not available"));
    return std::filesystem::path{home} / "Library" / "Application Support" / "axkdeck" / "workspaces.json";
#else
    if (const auto *base = std::getenv("XDG_CONFIG_HOME"); base != nullptr && *base != '\0')
        return std::filesystem::path{base} / "axkdeck" / "workspaces.json";
    const auto *home = std::getenv("HOME");
    if (home == nullptr || *home == '\0')
        return std::unexpected(workspace_error("workspace_store_unavailable", "HOME is not available"));
    return std::filesystem::path{home} / ".config" / "axkdeck" / "workspaces.json";
#endif
}

axk::app::Result<axk::server::WorkspaceStore> axk::server::WorkspaceStore::open(std::filesystem::path path) {
    if (path.empty()) {
        auto resolved = default_path();
        if (!resolved)
            return std::unexpected(resolved.error());
        path = std::move(*resolved);
    }
    if (!path.is_absolute())
        return std::unexpected(workspace_error("workspace_store_invalid", "workspace store path must be absolute"));
    auto sandbox = app::Sandbox::create({});
    if (!sandbox)
        return std::unexpected(sandbox.error());
    auto state = std::make_shared<State>(std::move(path), std::move(*sandbox));
    std::error_code error;
    if (!std::filesystem::exists(state->path, error))
        return WorkspaceStore{std::move(state)};
    std::ifstream input{state->path, std::ios::binary};
    Json document;
    try {
        document = Json::parse(input);
        if (!document.is_object() || document.size() != 3U || document.at("schemaVersion") != 1U ||
            !document.at("revision").is_number_unsigned() || !document.at("workspaces").is_array()) {
            throw Json::type_error::create(302, "invalid workspace store", &document);
        }
        state->revision = document.at("revision").get<std::uint64_t>();
        std::set<std::string> identifiers;
        for (const auto &value : document.at("workspaces")) {
            if (!value.is_object() || value.size() != 4U)
                throw Json::type_error::create(302, "invalid workspace", &value);
            auto workspace_path = text::path_from_utf8(value.at("path").get<std::string>());
            if (!workspace_path)
                throw Json::type_error::create(302, "invalid workspace path", &value);
            WorkspaceDefinition definition{.id = value.at("id").get<std::string>(),
                                           .display_name = value.at("displayName").get<std::string>(),
                                           .path = std::move(*workspace_path),
                                           .writable = value.at("writable").get<bool>()};
            if (definition.id.empty() || definition.display_name.empty() || !definition.path.is_absolute() ||
                !identifiers.insert(definition.id).second) {
                throw Json::type_error::create(302, "invalid workspace fields", &value);
            }
            state->definitions.push_back(std::move(definition));
        }
    } catch (const std::exception &) {
        state->configuration_issue = "workspace store is unreadable or has an unsupported schema";
    }
    WorkspaceStore result{std::move(state)};
    static_cast<void>(result.snapshot());
    return result;
}

const std::filesystem::path &axk::server::WorkspaceStore::path() const noexcept { return state_->path; }

axk::app::Sandbox axk::server::WorkspaceStore::sandbox() const { return state_->sandbox; }

axk::server::WorkspaceSnapshot axk::server::WorkspaceStore::snapshot() {
    const std::lock_guard lock{state_->mutex};
    WorkspaceSnapshot result{.revision = state_->revision,
                             .state = WorkspaceConfigurationState::no_available_workspace,
                             .workspaces = {},
                             .configuration_issue = state_->configuration_issue};
    if (state_->configuration_issue) {
        result.state = WorkspaceConfigurationState::configuration_error;
        static_cast<void>(state_->sandbox.replace_roots({}));
        return result;
    }
    std::vector<app::RootDefinition> active;
    for (const auto &definition : state_->definitions) {
        auto info = inspect(definition);
        if (info.status == WorkspaceStatus::available || info.status == WorkspaceStatus::read_only) {
            active.push_back({.id = info.definition.id,
                              .display_name = info.definition.display_name,
                              .path = info.definition.path,
                              .writable = info.effective_writable});
        }
        result.workspaces.push_back(std::move(info));
    }
    if (auto replaced = state_->sandbox.replace_roots(std::move(active)); !replaced) {
        result.state = WorkspaceConfigurationState::configuration_error;
        result.configuration_issue = replaced.error().message;
        return result;
    }
    result.state =
        result.workspaces.empty() || std::ranges::none_of(result.workspaces,
                                                          [](const WorkspaceInfo &info) {
                                                              return info.status == WorkspaceStatus::available ||
                                                                     info.status == WorkspaceStatus::read_only;
                                                          })
            ? WorkspaceConfigurationState::no_available_workspace
            : WorkspaceConfigurationState::ready;
    return result;
}

axk::app::Result<axk::server::WorkspaceInfo> axk::server::WorkspaceStore::add(std::string display_name,
                                                                              std::filesystem::path path, bool writable,
                                                                              std::uint64_t expected_revision) {
    const std::lock_guard lock{state_->mutex};
    if (state_->configuration_issue)
        return std::unexpected(
            workspace_error("workspace_store_recovery_required", "workspace store must be reset explicitly"));
    if (expected_revision != state_->revision)
        return std::unexpected(workspace_error("workspace_revision_conflict", "workspace configuration changed"));
    if (display_name.empty() || !path.is_absolute())
        return std::unexpected(workspace_error("invalid_workspace", "workspace name and absolute path are required"));
    auto id = app::secure_random_hex(8U);
    if (!id)
        return std::unexpected(id.error());
    WorkspaceDefinition definition{.id = "workspace-" + *id,
                                   .display_name = std::move(display_name),
                                   .path = std::move(path),
                                   .writable = writable};
    const auto info = inspect(definition);
    if (info.status != WorkspaceStatus::available && info.status != WorkspaceStatus::read_only)
        return std::unexpected(workspace_error("invalid_workspace", info.issue.value_or("workspace is unavailable")));
    auto updated = state_->definitions;
    updated.push_back(definition);
    if (auto written = publish(state_->path, state_->revision + 1U, updated); !written)
        return std::unexpected(written.error());
    state_->definitions = std::move(updated);
    ++state_->revision;
    return info;
}

axk::app::Result<axk::server::WorkspaceInfo>
axk::server::WorkspaceStore::update(std::string_view id, std::optional<std::string> display_name,
                                    std::optional<std::filesystem::path> path, std::optional<bool> writable,
                                    std::uint64_t expected_revision) {
    const std::lock_guard lock{state_->mutex};
    if (state_->configuration_issue)
        return std::unexpected(
            workspace_error("workspace_store_recovery_required", "workspace store must be reset explicitly"));
    if (expected_revision != state_->revision)
        return std::unexpected(workspace_error("workspace_revision_conflict", "workspace configuration changed"));
    auto updated = state_->definitions;
    const auto found = std::ranges::find(updated, id, &WorkspaceDefinition::id);
    if (found == updated.end())
        return std::unexpected(workspace_error("workspace_not_found", "workspace does not exist"));
    if (display_name)
        found->display_name = std::move(*display_name);
    if (path)
        found->path = std::move(*path);
    if (writable)
        found->writable = *writable;
    if (found->display_name.empty() || !found->path.is_absolute())
        return std::unexpected(workspace_error("invalid_workspace", "workspace name and absolute path are required"));
    const auto info = inspect(*found);
    if (auto written = publish(state_->path, state_->revision + 1U, updated); !written)
        return std::unexpected(written.error());
    state_->definitions = std::move(updated);
    ++state_->revision;
    return info;
}

axk::app::Result<void> axk::server::WorkspaceStore::remove(std::string_view id, std::uint64_t expected_revision) {
    const std::lock_guard lock{state_->mutex};
    if (state_->configuration_issue)
        return std::unexpected(
            workspace_error("workspace_store_recovery_required", "workspace store must be reset explicitly"));
    if (expected_revision != state_->revision)
        return std::unexpected(workspace_error("workspace_revision_conflict", "workspace configuration changed"));
    auto updated = state_->definitions;
    const auto found = std::ranges::find(updated, id, &WorkspaceDefinition::id);
    if (found == updated.end())
        return std::unexpected(workspace_error("workspace_not_found", "workspace does not exist"));
    updated.erase(found);
    if (auto written = publish(state_->path, state_->revision + 1U, updated); !written)
        return std::unexpected(written.error());
    state_->definitions = std::move(updated);
    ++state_->revision;
    return {};
}

axk::app::Result<std::optional<std::filesystem::path>> axk::server::WorkspaceStore::archive_and_reset() {
    const std::lock_guard lock{state_->mutex};
    if (!state_->configuration_issue) {
        return std::unexpected(workspace_error("workspace_store_not_corrupt",
                                               "workspace store recovery is available only for an unreadable store"));
    }
    std::optional<std::filesystem::path> archived;
    std::error_code error;
    if (std::filesystem::exists(state_->path, error)) {
        const auto stamp =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        archived =
            state_->path.parent_path() / (state_->path.filename().string() + ".corrupt-" + std::to_string(stamp));
        std::filesystem::rename(state_->path, *archived, error);
        if (error)
            return std::unexpected(
                workspace_error("workspace_store_recovery_failed", "workspace store cannot be archived"));
    }
    if (auto written = publish(state_->path, 0U, {}); !written)
        return std::unexpected(written.error());
    state_->revision = 0U;
    state_->definitions.clear();
    state_->configuration_issue.reset();
    static_cast<void>(state_->sandbox.replace_roots({}));
    return archived;
}
