#include "local_operations.hpp"

#include "exit_status.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <iostream>
#include <optional>
#include <random>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "axklib/application/application_operations.hpp"
#include "axklib/application/filesystem.hpp"
#include "axklib/application/uploads.hpp"
#include "axklib/utf8.hpp"

namespace {

axk::app::Error local_error(std::string code, std::string message,
                            std::optional<std::string> relative_path = std::nullopt) {
    axk::app::ErrorContext context;
    context.relative_path = std::move(relative_path);
    return {std::move(code), std::move(message), std::move(context)};
}

axk::app::Result<std::filesystem::path> normalized_absolute(const std::filesystem::path &path) {
    if (path.empty())
        return std::unexpected(local_error("invalid_local_path", "local path must not be empty"));
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error)
        return std::unexpected(local_error("invalid_local_path", "local path cannot be made absolute"));
    return absolute.lexically_normal();
}

axk::app::Result<std::string> generic_relative_utf8(const std::filesystem::path &path) {
    std::string result;
    for (const auto &component : path) {
        if (component == "." || component == "..")
            return std::unexpected(local_error("invalid_local_path", "local path is not normalized"));
        auto text = axk::text::path_to_utf8(component);
        if (text.empty() || text == "/" || text == "\\")
            continue;
        if (text.find('/') != std::string::npos || text.find('\\') != std::string::npos ||
            text.find(':') != std::string::npos) {
            return std::unexpected(local_error("invalid_local_path", "local path component is not portable"));
        }
        if (!result.empty())
            result.push_back('/');
        result += text;
    }
    if (result.empty())
        return std::unexpected(local_error("invalid_local_path", "local path must name an entry"));
    return result;
}

axk::app::Result<std::filesystem::path> reserve_staging_directory() {
    std::error_code error;
    const auto parent = std::filesystem::temp_directory_path(error);
    if (error)
        return std::unexpected(local_error("local_staging_unavailable", "temporary directory is unavailable"));

    std::random_device source;
    std::uniform_int_distribution<std::uint64_t> distribution;
    for (std::size_t attempt = 0; attempt < 32U; ++attempt) {
        const auto path = parent / std::format("axklib-cli-{:016x}", distribution(source));
        if (std::filesystem::create_directory(path, error))
            return path;
        if (error && error != std::errc::file_exists)
            break;
        error.clear();
    }
    return std::unexpected(local_error("local_staging_unavailable", "temporary staging directory cannot be created"));
}

} // namespace

struct axk::cli::LocalOperationRuntime::RootMapping {
    std::string id;
    std::filesystem::path canonical_path;
};

axk::cli::LocalOperationRuntime::LocalOperationRuntime(std::vector<RootMapping> roots,
                                                       std::filesystem::path staging_directory,
                                                       std::map<std::filesystem::path, std::string> display_paths,
                                                       std::unique_ptr<app::Sandbox> sandbox,
                                                       std::unique_ptr<app::UploadStore> uploads,
                                                       app::OperationRegistry registry)
    : roots_(std::move(roots)), staging_directory_(std::move(staging_directory)),
      display_paths_(std::move(display_paths)), sandbox_(std::move(sandbox)), uploads_(std::move(uploads)),
      registry_(std::move(registry)) {}

axk::cli::LocalOperationRuntime::~LocalOperationRuntime() {
    uploads_.reset();
    std::error_code error;
    std::filesystem::remove_all(staging_directory_, error);
}

axk::app::Result<std::unique_ptr<axk::cli::LocalOperationRuntime>>
axk::cli::LocalOperationRuntime::create(std::span<const std::filesystem::path> paths) {
    if (paths.empty())
        return std::unexpected(local_error("invalid_local_path", "at least one local path is required"));

    std::vector<RootMapping> mappings;
    std::vector<app::RootDefinition> definitions;
    std::map<std::filesystem::path, std::string> display_paths;
    for (const auto &path : paths) {
        auto absolute = normalized_absolute(path);
        if (!absolute)
            return std::unexpected(absolute.error());
        std::error_code display_error;
        const auto display_key = std::filesystem::weakly_canonical(*absolute, display_error);
        if (!display_error)
            display_paths.try_emplace(display_key, axk::text::path_to_utf8(path));
        const auto root_path = absolute->root_path();
        if (root_path.empty())
            return std::unexpected(local_error("invalid_local_path", "local path has no filesystem root"));
        std::error_code error;
        const auto canonical = std::filesystem::canonical(root_path, error);
        if (error)
            return std::unexpected(local_error("invalid_local_path", "local filesystem root is unavailable"));
        if (std::ranges::find(mappings, canonical, &RootMapping::canonical_path) != mappings.end())
            continue;
        const auto id = std::format("local-{}", mappings.size());
        mappings.push_back({id, canonical});
        definitions.push_back({id, "Local filesystem", canonical, true});
    }

    auto staging = reserve_staging_directory();
    if (!staging)
        return std::unexpected(staging.error());
    definitions.push_back({"cli-staging", "CLI staging", *staging, true});
    auto sandbox = app::Sandbox::create(std::move(definitions));
    if (!sandbox) {
        std::error_code error;
        std::filesystem::remove_all(*staging, error);
        return std::unexpected(sandbox.error());
    }
    auto sandbox_pointer = std::make_unique<app::Sandbox>(std::move(*sandbox));
    auto uploads =
        std::make_unique<app::UploadStore>(*staging, 1U << 30U, 1U << 30U, 64U, 8U << 20U, std::chrono::minutes{30});
    auto registry = app::make_application_registry(*sandbox_pointer, *uploads);
    if (!registry) {
        std::error_code error;
        std::filesystem::remove_all(*staging, error);
        return std::unexpected(registry.error());
    }
    return std::unique_ptr<LocalOperationRuntime>{
        new LocalOperationRuntime{std::move(mappings), std::move(*staging), std::move(display_paths),
                                  std::move(sandbox_pointer), std::move(uploads), std::move(*registry)}};
}

axk::app::Result<axk::app::FileRef>
axk::cli::LocalOperationRuntime::reference(const std::filesystem::path &path) const {
    auto absolute = normalized_absolute(path);
    if (!absolute)
        return std::unexpected(absolute.error());
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(*absolute, error);
    if (error)
        return std::unexpected(local_error("invalid_local_path", "local path cannot be resolved"));
    for (const auto &root : roots_) {
        const auto relative = canonical.lexically_relative(root.canonical_path);
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
            continue;
        auto encoded = generic_relative_utf8(relative);
        if (!encoded)
            return std::unexpected(encoded.error());
        return app::FileRef{root.id, std::move(*encoded)};
    }
    return std::unexpected(local_error("invalid_local_path", "local path is outside the selected filesystem roots"));
}

axk::app::Result<axk::app::FileRef> axk::cli::LocalOperationRuntime::file_ref(const std::filesystem::path &path) const {
    return reference(path);
}

axk::app::Result<axk::app::DirectoryRef>
axk::cli::LocalOperationRuntime::directory_ref(const std::filesystem::path &path) const {
    auto file = reference(path);
    if (!file)
        return std::unexpected(file.error());
    return app::DirectoryRef{std::move(file->root_id), std::move(file->relative_path)};
}

axk::app::FileRef axk::cli::LocalOperationRuntime::scratch_file_ref(std::string filename) const {
    return {"cli-staging", std::move(filename)};
}

axk::app::Result<std::filesystem::path>
axk::cli::LocalOperationRuntime::resolve_file(const app::FileRef &reference) const {
    return sandbox_->resolve_file(reference);
}

std::string axk::cli::LocalOperationRuntime::display_path(const app::FileRef &reference) const {
    auto resolved = sandbox_->resolve_file(reference);
    if (!resolved)
        return reference.relative_path;
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(*resolved, error);
    if (!error) {
        if (const auto found = display_paths_.find(canonical); found != display_paths_.end())
            return found->second;
    }
    return reference.relative_path;
}

axk::app::Result<nlohmann::json> axk::cli::LocalOperationRuntime::invoke(std::string_view operation_id,
                                                                         const nlohmann::json &input) const {
    return registry_.invoke(
        operation_id, input,
        {.owner_id = "cli",
         .request_id = "cli",
         .cancellation = {},
         .progress = nullptr,
         .display_path = [this](const app::FileRef &reference) { return display_path(reference); }});
}

int axk::cli::report_application_failure(const app::Error &error) {
    std::cerr << error.code << ": " << error.message;
    if (error.context.relative_path)
        std::cerr << " [path=" << *error.context.relative_path << ']';
    std::cerr << '\n';
    return exit_code(application_error_status(error.code));
}
