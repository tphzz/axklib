#pragma once

#include "package_operations_internal.hpp"

namespace axk::app::package_operations_internal {
[[nodiscard]] Result<Json> store_session_import_plan(const std::shared_ptr<SessionPackageOperationState> &state,
                                                     const Json &input, const OperationContext &context,
                                                     const ImageSessionRead &session,
                                                     const std::shared_ptr<const VerifiedPackageSet> &package_set,
                                                     const std::optional<std::string> &replace_plan_token = {},
                                                     Clock::time_point started = Clock::now());
} // namespace axk::app::package_operations_internal
