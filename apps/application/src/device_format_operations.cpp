#include "device_format.hpp"

namespace axk::app {
Result<void> bind_device_format_operations(OperationRegistry &registry, DeviceSessionManager &devices) {
    auto service = std::make_shared<DeviceFormatService>(devices);
    if (auto bound = registry.bind(
            "devices.inspect",
            [service](const auto &input, const auto &context) { return service->inspect(input, context); });
        !bound)
        return bound;
    if (auto bound =
            registry.bind("devices.sfs.plan",
                          [service](const auto &input, const auto &context) { return service->plan(input, context); });
        !bound)
        return bound;
    if (auto bound = registry.bind(
            "devices.sfs.format",
            [service](const auto &input, const auto &context) { return service->execute(input, context); });
        !bound)
        return bound;
    return registry.bind_job_admission("devices.sfs.format", [service](const auto &input, const auto &context) {
        return service->admit(input, context);
    });
}
} // namespace axk::app
