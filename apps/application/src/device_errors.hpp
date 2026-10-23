#pragma once

#include "axklib/application/contracts.hpp"
#include "rawdisk/errors.hpp"

#include <system_error>

namespace axk::app::device_detail {

inline Error native_error(const std::error_code &error) {
    if (error == rawdisk::Errc::device_identity_unavailable)
        return {"device_identity_unavailable", error.message()};
    if (error == rawdisk::Errc::device_connection_changed)
        return {"device_connection_changed", error.message()};
    if (error == rawdisk::Errc::device_descriptor_changed)
        return {"device_descriptor_changed", error.message()};
    if (error == rawdisk::Errc::device_geometry_changed)
        return {"device_geometry_changed", error.message()};
    return {"device_access_failed", error.message()};
}

} // namespace axk::app::device_detail
