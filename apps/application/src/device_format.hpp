#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include "axklib/application/device_sessions.hpp"
#include "axklib/application/operation_registry.hpp"

namespace axk::app {
class DeviceFormatService {
  public:
    using Now = std::function<std::chrono::steady_clock::time_point()>;
    explicit DeviceFormatService(
        DeviceSessionManager &devices, Now now = [] { return std::chrono::steady_clock::now(); });
    Result<nlohmann::json> inspect(const nlohmann::json &input, const OperationContext &context);
    Result<nlohmann::json> plan(const nlohmann::json &input, const OperationContext &context);
    Result<std::shared_ptr<void>> admit(const nlohmann::json &input, const OperationContext &context);
    Result<nlohmann::json> execute(const nlohmann::json &input, const OperationContext &context);

  private:
    struct State;
    std::shared_ptr<State> state_;
};
Result<void> bind_device_format_operations(OperationRegistry &registry, DeviceSessionManager &devices);
} // namespace axk::app
