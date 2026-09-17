#pragma once

#include <functional>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/application/operation_registry.hpp"
#include "axklib/application/uploads.hpp"
#include "axklib/floppy_import.hpp"

namespace axk::app {
struct FloppyDirectorySources {
    std::vector<FloppyImportDirectory> members;
    std::function<Result<void>(const CancellationToken &)> verify;
};

[[nodiscard]] Result<FloppyDirectorySources> open_floppy_directories(const nlohmann::json &sources,
                                                                     const OperationContext &context,
                                                                     const Sandbox &sandbox, UploadStore &uploads);
} // namespace axk::app
