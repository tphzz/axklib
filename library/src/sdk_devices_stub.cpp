#include "axklib/sdk/devices.hpp"

namespace axk {
namespace {
error unavailable() {
    return {error_code::unsupported_profile,
            error_category::unsupported,
            "device support is not included in this SDK build",
            {}};
}
} // namespace
struct device_host::impl {};
device_host::device_host() = default;
device_host::~device_host() = default;
device_host::device_host(device_host &&) noexcept = default;
device_host &device_host::operator=(device_host &&) noexcept = default;
bool device_host::available() noexcept { return false; }
result<std::vector<scsi2sd_xml_target>> inspect_scsi2sd_xml(const std::string &) { return unavailable(); }
result<device_host> device_host::create(const std::string &, const std::string &) { return unavailable(); }
result<std::vector<device_candidate>> device_host::list() { return unavailable(); }
result<device_session> device_host::connect(const std::string &, const std::string &, const std::string &) {
    return unavailable();
}
result<device_session> device_host::status(const std::string &) { return unavailable(); }
result<image> device_host::open_image(const std::string &, const std::string &, operation_context &) {
    return unavailable();
}
result<void> device_host::authorize_writes(const std::string &, const std::string &, bool) { return unavailable(); }
result<device_operation> device_host::apply(const std::string &, const std::string &, const std::string &,
                                            const std::string &, operation_context &) {
    return unavailable();
}
result<std::vector<device_operation>> device_host::operations() { return unavailable(); }
result<void> device_host::recover(const std::string &, const std::string &, const std::string &, bool) {
    return unavailable();
}
result<void> device_host::close(const std::string &) { return unavailable(); }
} // namespace axk
