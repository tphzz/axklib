#pragma once

#include <filesystem>
#include <memory>

#include "axklib/application/device_sessions.hpp"
#include "axklib/sdk/devices.hpp"

namespace axk {

struct device_host_factory {
    static result<device_host> create(const std::filesystem::path &directory,
                                      std::shared_ptr<app::DeviceProvider> provider, bool writes_qualified);
};

} // namespace axk
