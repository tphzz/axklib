#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "schema/package_v1.hpp"

namespace axk::cli::schema::package_v1 {

Result<PackageOutput> project_package(const std::filesystem::path &path, const nlohmann::json &service_result);
Result<PlanOutput> project_plan(const std::filesystem::path &target,
                                const std::vector<std::filesystem::path> &package_paths,
                                const nlohmann::json &service_plan,
                                const std::optional<std::filesystem::path> &output_path,
                                const nlohmann::json *service_result);

} // namespace axk::cli::schema::package_v1
