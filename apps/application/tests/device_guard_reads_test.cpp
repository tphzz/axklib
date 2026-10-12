#include "device_sessions_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace axk::app;
using namespace axk::app::device_detail;

class GuardTransport final : public DeviceTransport {
  public:
    std::vector<std::byte> bytes = std::vector<std::byte>(3U * 1024U * 1024U, std::byte{0x42});
    mutable std::vector<std::pair<std::uint64_t, std::size_t>> reads;
    std::function<void()> before_read;
    bool fail_reads{};
    std::uint64_t size() const noexcept override { return bytes.size(); }
    std::uint32_t sector_size() const noexcept override { return 512U; }
    std::string carrier_identity() const override { return "guard-carrier"; }
    std::string media_identity() const override { return "guard-media"; }
    Result<void> authorize_writes() override { return std::unexpected(Error{"test_read_only", "read-only test"}); }
    Result<void> verify_bound() const override { return {}; }
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> data) const override {
        reads.emplace_back(offset, data.size());
        if (before_read)
            before_read();
        if (fail_reads || offset > size() || data.size() > size() - offset)
            return std::unexpected(
                axk::make_error(axk::ErrorCode::io_read_failed, axk::ErrorCategory::io, "test read failed"));
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), data.size(), data.begin());
        return {};
    }
    Result<void> write_exact_at(std::uint64_t, std::span<const std::byte>) override {
        ADD_FAILURE() << "Guard verification must never write";
        return std::unexpected(Error{"test_read_only", "read-only test"});
    }
    Result<void> flush() override { return {}; }
};

class GuardProgress final : public axk::ProgressSink {
  public:
    std::vector<axk::Progress> events;
    void report(const axk::Progress &progress) noexcept override { events.push_back(progress); }
};

class DeviceGuardReads : public testing::Test {
  protected:
    std::shared_ptr<GuardTransport> transport = std::make_shared<GuardTransport>();
    Connection connection;
    DeviceTargetDescription target{"target", "Target", 4096U, 2U * 1024U * 1024U, 512U, false, {}, {}};
    Guards guards;
    void SetUp() override { connection.device.transport = transport; }
    void guard(std::uint64_t offset) {
        guards.emplace(offset, *digest(std::span{transport->bytes}.subspan(
                                   static_cast<std::size_t>(target.offset + offset), 512U)));
    }
};

TEST_F(DeviceGuardReads, ContiguousBatchesAreLimitedToOneMiBAndTheTargetSlice) {
    for (std::uint64_t offset = 0U; offset < target.size_bytes; offset += 512U)
        guard(offset);
    GuardProgress progress;
    ASSERT_TRUE(verify_guards(connection, target, guards, {}, {}, &progress));
    ASSERT_EQ(transport->reads.size(), 2U);
    for (const auto &[offset, size] : transport->reads) {
        EXPECT_LE(size, 1024U * 1024U);
        EXPECT_GE(offset, target.offset);
        EXPECT_LE(offset + size, target.offset + target.size_bytes);
    }
    EXPECT_EQ(connection.reads.verification_passes, 1U);
    EXPECT_EQ(connection.reads.verification_reads, 2U);
    EXPECT_EQ(connection.reads.verified_bytes, target.size_bytes);
    ASSERT_FALSE(progress.events.empty());
    EXPECT_EQ(progress.events.back().completed, target.size_bytes);
    EXPECT_EQ(progress.events.back().total, target.size_bytes);
}

TEST_F(DeviceGuardReads, ExcludedPatchSectorsAreNeverReadOrCrossedByABatch) {
    for (const auto offset : {0U, 512U, 1024U, 1536U})
        guard(offset);
    transport->bytes[static_cast<std::size_t>(target.offset + 512U)] = std::byte{0x99};
    const std::vector<AlterationJournalPatch> excluded{
        {512U, std::vector<std::byte>(512U), std::vector<std::byte>(512U)},
        {1536U, std::vector<std::byte>(512U), std::vector<std::byte>(512U)}};
    GuardProgress progress;
    ASSERT_TRUE(verify_guards(connection, target, guards, excluded, {}, &progress));
    ASSERT_EQ(transport->reads.size(), 2U);
    EXPECT_EQ(transport->reads[0], (std::pair<std::uint64_t, std::size_t>{target.offset, 512U}));
    EXPECT_EQ(transport->reads[1], (std::pair<std::uint64_t, std::size_t>{target.offset + 1024U, 512U}));
    EXPECT_EQ(connection.reads.verified_bytes, 1024U);
    ASSERT_FALSE(progress.events.empty());
    EXPECT_EQ(progress.events.back().completed, 2048U);
    EXPECT_EQ(progress.events.back().total, 2048U);
}

TEST_F(DeviceGuardReads, EveryPassReadsFreshBytesAndPropagatesReadFailures) {
    guard(0U);
    guard(512U);
    ASSERT_TRUE(verify_guards(connection, target, guards));
    transport->bytes[static_cast<std::size_t>(target.offset + 512U)] = std::byte{0x99};
    const auto changed = verify_guards(connection, target, guards);
    ASSERT_FALSE(changed);
    EXPECT_EQ(changed.error().code, "device_changed");
    EXPECT_EQ(transport->reads.size(), 2U);
    transport->fail_reads = true;
    const auto failed = verify_guards(connection, target, guards);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, "device_changed");
    EXPECT_EQ(connection.reads.verification_reads, 3U);
}

TEST_F(DeviceGuardReads, InvalidGuardOffsetsAreRejectedWithoutDeviceReads) {
    const auto hash = *digest(std::span{transport->bytes}.first(512U));
    for (const auto offset :
         std::array<std::uint64_t, 3U>{1U, target.size_bytes, std::numeric_limits<std::uint64_t>::max() - 511U}) {
        SCOPED_TRACE(offset);
        guards = {{offset, hash}};
        EXPECT_FALSE(verify_guards(connection, target, guards));
        EXPECT_TRUE(transport->reads.empty());
    }
}

TEST_F(DeviceGuardReads, CancellationBeforeOrDuringAPassPreventsFurtherReads) {
    guard(0U);
    guard(target.size_bytes - 512U);
    axk::CancellationSource cancelled;
    cancelled.cancel();
    const auto before = verify_guards(connection, target, guards, {}, cancelled.token());
    ASSERT_FALSE(before);
    EXPECT_EQ(before.error().code, "operation_cancelled");
    EXPECT_TRUE(transport->reads.empty());
    axk::CancellationSource during;
    transport->before_read = [&] { during.cancel(); };
    const auto interrupted = verify_guards(connection, target, guards, {}, during.token());
    ASSERT_FALSE(interrupted);
    EXPECT_EQ(interrupted.error().code, "operation_cancelled");
    EXPECT_EQ(transport->reads.size(), 1U);
}
} // namespace
