#include "axklib/application/device_sessions.hpp"

#include <gtest/gtest.h>

namespace axk::app {
namespace {

TEST(DevicePlatform, CompiledProviderAvailabilityDoesNotEnumerateHardware) {
    const auto provider = native_device_provider();
    ASSERT_TRUE(provider);
#if defined(AXK_BUILD_DEVICES)
    EXPECT_TRUE(provider->available());
#else
    EXPECT_FALSE(provider->available());
#endif
}

TEST(DevicePlatform, NativeWriteAdmissionMatchesTheSupportedPlatformMatrix) {
#if defined(AXK_BUILD_DEVICES) &&                                                                                      \
    ((defined(__APPLE__) && (defined(__x86_64__) || defined(__aarch64__))) ||                                          \
     ((defined(__linux__) || defined(_WIN32)) && (defined(__x86_64__) || defined(_M_X64))))
    EXPECT_TRUE(native_device_writes_qualified());
#else
    EXPECT_FALSE(native_device_writes_qualified());
#endif
}

} // namespace
} // namespace axk::app
