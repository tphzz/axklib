#pragma once

#include <string>

#include "axklib/package.hpp"

namespace axk::package_import_internal {

std::string plan_identity(const PackageImportPlan &plan);
std::string program_assignment_adjustment_identity(const PackageProgramAssignmentAdjustment &adjustment);

} // namespace axk::package_import_internal
