#pragma once

#include "axklib/conversion/plan.hpp"
#include "conversion_sources.hpp"

namespace axk::app::conversion_internal {
[[nodiscard]] Result<nlohmann::json>
inspect_conversion_source(const nlohmann::json &input, const conversion::Registry &profiles,
                          const native::FormatRegistry &formats, const Sandbox &sandbox, UploadStore &uploads,
                          ImageSessionManager &images, const OperationContext &context);
}
