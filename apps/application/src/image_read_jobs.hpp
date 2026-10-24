#pragma once

#include "axklib/application/image_sessions.hpp"
#include "axklib/application/operation_registry.hpp"

namespace axk::app {
enum class ImageReadJob { preview, audition };
Result<std::optional<ReadJobScheduling>> image_read_scheduling(ImageSessionManager &images,
                                                               const nlohmann::json &request,
                                                               const OperationContext &context, ImageReadJob kind);
Result<void> bind_preview_operation(OperationRegistry &registry, ImageSessionManager &images);
} // namespace axk::app
