#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "axklib/server/process_lifetime.hpp"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

std::uint64_t current_process_id() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

TEST(ServerProcessLifetime, DistinguishesTheCurrentProcessFromAnInvalidProcess) {
    EXPECT_TRUE(axk::server::process_is_running(current_process_id()));
    EXPECT_FALSE(axk::server::process_is_running(0U));
    EXPECT_FALSE(axk::server::process_is_running(std::numeric_limits<std::uint64_t>::max()));
}

} // namespace
