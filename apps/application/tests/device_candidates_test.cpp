#include "device_candidates.hpp"

#include <array>
#include <expected>
#include <functional>
#include <map>
#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace axk::app {
namespace {

rawdisk::DeviceInfo device(std::string connection = "connection-1") {
    return {.path = "/dev/test",
            .model = "Test",
            .serial = "duplicate-serial",
            .size_bytes = 1024U,
            .sector_size = 512U,
            .removable = true,
            .carrier_id = "carrier",
            .connection_id = std::move(connection)};
}

TEST(DeviceCandidates, RetainsIdsAcrossEnumerationOrderChanges) {
    std::map<std::string, rawdisk::DeviceInfo, std::less<>> current;
    unsigned sequence{};
    auto generate = [&]() -> Result<std::string> { return std::to_string(++sequence); };
    auto first = device();
    auto second = device("connection-2");
    second.path = "/dev/other";
    auto listed = device_detail::refresh_candidates(current, std::array{first, second}, generate);
    ASSERT_TRUE(listed);
    const auto original = *listed;
    listed = device_detail::refresh_candidates(current, std::array{second, first}, generate);
    ASSERT_TRUE(listed);
    EXPECT_EQ(listed->at(0).id, original.at(1).id);
    EXPECT_EQ(listed->at(1).id, original.at(0).id);
    EXPECT_EQ(sequence, 2U);
}

TEST(DeviceCandidates, ChangesIdsForNewConnectionsAndAfterObservedRemoval) {
    std::map<std::string, rawdisk::DeviceInfo, std::less<>> current;
    unsigned sequence{};
    auto generate = [&]() -> Result<std::string> { return std::to_string(++sequence); };
    auto listed = device_detail::refresh_candidates(current, std::array{device()}, generate);
    ASSERT_TRUE(listed);
    const auto original = listed->front().id;
    listed = device_detail::refresh_candidates(current, std::array{device("connection-2")}, generate);
    ASSERT_TRUE(listed);
    EXPECT_NE(listed->front().id, original);
    const auto replacement = listed->front().id;
    ASSERT_TRUE(device_detail::refresh_candidates(current, {}, generate));
    EXPECT_TRUE(current.empty());
    listed = device_detail::refresh_candidates(current, std::array{device("connection-2")}, generate);
    ASSERT_TRUE(listed);
    EXPECT_NE(listed->front().id, replacement);
}

TEST(DeviceCandidates, FailedPublicationRetainsThePreviousMap) {
    const auto original = device();
    std::map<std::string, rawdisk::DeviceInfo, std::less<>> current{{"original", original}};
    const auto failed =
        device_detail::refresh_candidates(current, std::array{device("new")}, []() -> Result<std::string> {
            return std::unexpected(Error{"random_failed", "injected failure"});
        });
    EXPECT_FALSE(failed);
    ASSERT_EQ(current.size(), 1U);
    EXPECT_EQ(current.at("original"), original);
}

TEST(DeviceCandidates, DoesNotRetainIdsWhenNativeIdentityOrGeometryChanges) {
    const auto original = device();
    std::array changed{original, original, original, original};
    changed[0].path = "/dev/other";
    changed[1].size_bytes = 2048U;
    changed[2].sector_size = 1024U;
    changed[3].carrier_id = "other-carrier";
    for (const auto &value : changed) {
        std::map<std::string, rawdisk::DeviceInfo, std::less<>> current{{"original", original}};
        const auto listed = device_detail::refresh_candidates(current, std::array{value},
                                                              []() -> Result<std::string> { return "new"; });
        ASSERT_TRUE(listed);
        EXPECT_EQ(listed->front().id, "new");
        EXPECT_FALSE(current.contains("original"));
    }
}

TEST(DeviceCandidates, RefusesGeneratedIdsThatCollideWithPreviousConnections) {
    const auto original = device();
    std::map<std::string, rawdisk::DeviceInfo, std::less<>> current{{"original", original}};
    const auto listed = device_detail::refresh_candidates(current, std::array{device("new")},
                                                          []() -> Result<std::string> { return "original"; });
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().code, "device_identity_unavailable");
    EXPECT_EQ(current.at("original"), original);
}

} // namespace
} // namespace axk::app
