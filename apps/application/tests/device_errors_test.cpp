#include "device_errors.hpp"

#include <array>
#include <string_view>
#include <system_error>
#include <utility>

#include <gtest/gtest.h>

namespace axk::app::device_detail {
namespace {

TEST(DeviceErrors, PreservesNativeIdentityFailureReasons) {
    constexpr std::array<std::pair<rawdisk::Errc, std::string_view>, 4> reasons{{
        {rawdisk::Errc::device_identity_unavailable, "device_identity_unavailable"},
        {rawdisk::Errc::device_connection_changed, "device_connection_changed"},
        {rawdisk::Errc::device_descriptor_changed, "device_descriptor_changed"},
        {rawdisk::Errc::device_geometry_changed, "device_geometry_changed"},
    }};
    for (const auto &[reason, code] : reasons) {
        const auto error = native_error(make_error_code(reason));
        EXPECT_EQ(error.code, code);
        EXPECT_FALSE(error.message.empty());
        EXPECT_FALSE(error.retryable);
    }
}

TEST(DeviceErrors, RetainsAccessFailureForOtherNativeAndOsErrors) {
    for (const auto error :
         {make_error_code(rawdisk::Errc::device_changed), std::make_error_code(std::errc::permission_denied)}) {
        const auto mapped = native_error(error);
        EXPECT_EQ(mapped.code, "device_access_failed");
        EXPECT_EQ(mapped.message, error.message());
    }
}

} // namespace
} // namespace axk::app::device_detail
