#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

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

TEST(ServerProcessLifetime, PrivateShutdownSignalRequiresTheExactTokenAndARegularFile) {
    const auto directory =
        std::filesystem::temp_directory_path() / ("axk-shutdown-test-" + std::to_string(current_process_id()));
    std::filesystem::create_directories(directory);
    const auto connection = directory / "connection.json";
    const auto signal = directory / "connection.shutdown";
    EXPECT_FALSE(axk::server::desktop_shutdown_requested(connection, "secret-token"));
    {
        std::ofstream file{signal, std::ios::binary};
        file << "secret-token";
    }
    EXPECT_FALSE(axk::server::desktop_shutdown_requested(connection, "wrong-token!"));
    EXPECT_TRUE(axk::server::desktop_shutdown_requested(connection, "secret-token"));
    std::filesystem::remove(signal);
    std::filesystem::create_directory(signal);
    EXPECT_FALSE(axk::server::desktop_shutdown_requested(connection, "secret-token"));
    std::filesystem::remove_all(directory);
}

} // namespace
