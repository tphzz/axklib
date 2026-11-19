#pragma once

#include <cstdint>
#include <limits>

#include "axklib/application/image_sessions.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/application/uploads.hpp"
#include "axklib/native/registry.hpp"

namespace axk::app::conversion_internal {
struct SourceBudget {
    std::uint64_t preparing{};
    std::uint64_t retained{};
};

[[nodiscard]] Result<SourceBudget> source_budget(const NativeDocumentSnapshot &source, std::uint64_t limit);

[[nodiscard]] Result<NativeDocumentSnapshot>
open_source(const nlohmann::json &input, const native::FormatRegistry &formats, std::string_view format_id,
            const Sandbox &sandbox, UploadStore &uploads, ImageSessionManager &images, const OperationContext &context,
            std::uint64_t maximum_source_bytes = std::numeric_limits<std::uint64_t>::max());
} // namespace axk::app::conversion_internal
