#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string_view>

#include "filesystem_inputs.hpp"

namespace axk::app {
struct FilesystemImageLease {
    std::function<Result<void>(const CancellationToken &)> verify;
    std::function<Result<filesystem_inputs::OpenedInput>(std::string_view)> open;
};

class FilesystemImageInputs {
  public:
    FilesystemImageInputs(const Sandbox &sandbox, UploadStore &uploads,
                          std::function<std::chrono::steady_clock::time_point()> now = std::chrono::steady_clock::now);
    [[nodiscard]] Result<nlohmann::json> inspect(const nlohmann::json &source, const OperationContext &context);
    [[nodiscard]] Result<FilesystemImageLease> lease(std::string_view token, std::string_view owner);
    [[nodiscard]] Result<void> release(std::string_view token, std::string_view owner);

  private:
    struct State;
    std::shared_ptr<State> state_;
    const Sandbox &sandbox_;
    UploadStore &uploads_;
};

[[nodiscard]] Result<void> bind_filesystem_image_inputs(OperationRegistry &registry,
                                                        const std::shared_ptr<FilesystemImageInputs> &inputs);
} // namespace axk::app
