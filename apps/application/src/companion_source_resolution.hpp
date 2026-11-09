#pragma once

#include "conversion_sources.hpp"

namespace axk::app::conversion_internal {
[[nodiscard]] Result<nlohmann::json>
resolve_companion_sources(const nlohmann::json &input, const native::FormatRegistry &formats, const Sandbox &sandbox,
                          UploadStore &uploads, ImageSessionManager &images, const OperationContext &context);
}
