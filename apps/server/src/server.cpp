#include "axklib/server/server.hpp"

#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "axklib/server/state_lease.hpp"
#include "axklib/server/workspaces.hpp"
#include "server_application.hpp"

axk::app::Result<int> axk::server::run(const Config &config, app::OperationRegistry registry) {
    try {
        if (const auto valid = validate_config(config); !valid)
            return std::unexpected(valid.error());
        auto resolved_config = config;
        if (resolved_config.workspace_store.empty()) {
            auto workspace_path = WorkspaceStore::default_path();
            if (!workspace_path)
                return std::unexpected(workspace_path.error());
            resolved_config.workspace_store = std::move(*workspace_path);
        }
        if (resolved_config.state_directory.empty()) {
            resolved_config.state_directory = std::filesystem::temp_directory_path() / "axklib-server";
        }
        std::error_code path_error;
        resolved_config.workspace_store = std::filesystem::absolute(resolved_config.workspace_store, path_error);
        if (path_error)
            return std::unexpected(app::Error{"server_state_unavailable", "workspace store path is invalid"});
        resolved_config.state_directory = std::filesystem::absolute(resolved_config.state_directory, path_error);
        if (path_error)
            return std::unexpected(app::Error{"server_state_unavailable", "state directory path is invalid"});

        auto state_lease = StateNamespaceLease::acquire(
            {resolved_config.state_directory / ".axklib-server-owner.lock",
             resolved_config.workspace_store.parent_path() /
                 ("." + resolved_config.workspace_store.filename().string() + ".owner.lock")});
        if (!state_lease)
            return std::unexpected(state_lease.error());

        std::vector<std::filesystem::path> protected_paths{
            resolved_config.state_directory,
            resolved_config.workspace_store,
        };
        if (!resolved_config.connection_file.empty())
            protected_paths.push_back(resolved_config.connection_file);
        auto workspaces = WorkspaceStore::open(resolved_config.workspace_store, std::move(protected_paths));
        if (!workspaces)
            return std::unexpected(workspaces.error());
        detail::ServerApplication application{std::move(resolved_config), std::move(registry), std::move(*workspaces)};
        return application.run();
    } catch (const std::exception &error) {
        return std::unexpected(
            app::Error{"server_start_failed", "server initialization failed: " + std::string{error.what()}});
    } catch (...) {
        return std::unexpected(app::Error{"server_start_failed", "server initialization failed"});
    }
}
