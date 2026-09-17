#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "axklib/application/operation_registry.hpp"
#include "axklib/application/uploads.hpp"

namespace axk::app::filesystem_inputs {

struct OpenedInput {
    std::shared_ptr<const RandomAccessReader> reader;
    std::string revision;
    UploadLease upload_lease;
    std::function<Result<void>()> verify_identity;

    [[nodiscard]] Result<nlohmann::json> snapshot(const CancellationToken &cancellation = {}) const;
    [[nodiscard]] Result<void> verify(const nlohmann::json &expected, const CancellationToken &cancellation = {}) const;
};

[[nodiscard]] Result<OpenedInput> open(const nlohmann::json &source, std::string_view owner, const Sandbox &sandbox,
                                       UploadStore &uploads);
[[nodiscard]] Result<void> bind_inspection(OperationRegistry &registry, const Sandbox &sandbox, UploadStore &uploads);

} // namespace axk::app::filesystem_inputs
