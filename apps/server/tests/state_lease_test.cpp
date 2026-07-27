#include <filesystem>

#include <gtest/gtest.h>

#include "axklib/server/state_lease.hpp"

namespace {

class StateLeaseTest : public testing::Test {
  protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() / "axklib-server-state-lease-test";
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        std::filesystem::create_directories(root_);
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    std::filesystem::path root_;
};

TEST_F(StateLeaseTest, RejectsASecondOwnerUntilTheFirstLeaseIsReleased) {
    const auto state_lock = root_ / "state" / ".axklib-server-owner.lock";
    const auto catalog_lock = root_ / "config" / ".workspaces-owner.lock";
    {
        auto first = axk::server::StateNamespaceLease::acquire({state_lock, catalog_lock});
        ASSERT_TRUE(first) << first.error().message;

        auto second = axk::server::StateNamespaceLease::acquire({catalog_lock, state_lock});
        ASSERT_FALSE(second);
        EXPECT_EQ(second.error().code, "server_state_in_use");
    }
    auto after_release = axk::server::StateNamespaceLease::acquire({state_lock, catalog_lock});
    ASSERT_TRUE(after_release) << after_release.error().message;
}

} // namespace
