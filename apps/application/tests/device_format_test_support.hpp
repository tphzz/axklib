#pragma once

#include "axklib/application/device_sessions.hpp"
#include "axklib/application/secure_random.hpp"
#include "device_format.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace device_format_test {
using Json = nlohmann::json;
using axk::app::Result;

struct Disk {
    std::uint64_t size = 2U * 1024U * 1024U;
    std::vector<std::pair<std::uint64_t, std::vector<std::byte>>> changes;
    std::size_t writes{};
    std::size_t fail_at{};
    bool fail_restore{};
    bool valid = true;
    bool writable{};
    bool claimed{};
    std::size_t ordinary_authorizations{};
    std::size_t initialization_authorizations{};
    std::function<void()> on_authorize;
    axk::app::DeviceWriteSafety safety{
        {"/dev/synthetic", "Test manufacturer", "Test bridge", "usb-serial", "1234", "5678"},
        "current-membership",
        "recovery-membership",
        {},
        {},
        true,
        true};

    void put(std::uint64_t offset, std::span<const std::byte> bytes) {
        changes.emplace_back(offset, std::vector<std::byte>{bytes.begin(), bytes.end()});
    }
};

class Transport final : public axk::app::DeviceTransport {
  public:
    explicit Transport(std::shared_ptr<Disk> media, std::string identity = "carrier")
        : disk(std::move(media)), carrier(std::move(identity)) {}
    std::shared_ptr<Disk> disk;
    std::string carrier;
    std::uint64_t size() const noexcept override { return disk->size; }
    std::uint32_t sector_size() const noexcept override { return 512U; }
    std::string carrier_identity() const override { return carrier; }
    std::string media_identity() const override { return "stable-media"; }
    bool has_write_handle() const override { return disk->writable; }
    Result<void> authorize_writes() override {
        ++disk->ordinary_authorizations;
        disk->writable = true;
        return {};
    }
    Result<axk::app::DeviceWriteSafety> inspect_write_safety() const override { return disk->safety; }
    Result<void> authorize_initialization(const axk::app::DeviceWriteSafety &expected) override {
        ++disk->initialization_authorizations;
        if (disk->on_authorize)
            disk->on_authorize();
        if (disk->writable || !disk->safety.complete || !disk->safety.non_dismounting_claim_supported ||
            !disk->safety.mounted.empty() || disk->safety.membership != expected.membership)
            return std::unexpected(axk::app::Error{"device_busy", "Synthetic claim refused"});
        disk->writable = true;
        disk->claimed = true;
        return verify_bound();
    }
    Result<void> verify_initialization_claim() const override {
        if (!disk->claimed)
            return std::unexpected(axk::app::Error{"device_busy", "Synthetic claim lost"});
        return verify_bound();
    }
    Result<void> verify_bound() const override {
        if (!disk->valid)
            return std::unexpected(axk::app::Error{"device_changed", "Synthetic carrier changed"});
        return {};
    }
    Result<void> flush() override { return verify_bound(); }
    Result<void> write_exact_at(std::uint64_t offset, std::span<const std::byte> bytes) override {
        ++disk->writes;
        if (disk->fail_at != 0U &&
            (disk->writes == disk->fail_at || (disk->fail_restore && disk->writes > disk->fail_at))) {
            if (disk->writes == disk->fail_at)
                disk->put(offset, bytes.first(std::min<std::size_t>(512U, bytes.size())));
            return std::unexpected(axk::app::Error{"device_write_failed", "Injected synthetic write failure"});
        }
        if (!disk->claimed || offset > size() || bytes.size() > size() - offset)
            return std::unexpected(axk::app::Error{"device_write_failed", "Synthetic write outside claim"});
        disk->put(offset, bytes);
        return {};
    }
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> bytes) const override {
        if (offset > size() || bytes.size() > size() - offset)
            return std::unexpected(
                axk::make_error(axk::ErrorCode::out_of_bounds, axk::ErrorCategory::io, "Synthetic bounds"));
        std::ranges::fill(bytes, std::byte{0x42});
        for (const auto &[start, data] : disk->changes) {
            const auto begin = std::max(start, offset);
            const auto end = std::min(start + data.size(), offset + bytes.size());
            if (begin < end)
                std::memcpy(bytes.data() + begin - offset, data.data() + begin - start,
                            static_cast<std::size_t>(end - begin));
        }
        return {};
    }
};

class Provider final : public axk::app::DeviceProvider {
  public:
    std::shared_ptr<Disk> disk = std::make_shared<Disk>();
    std::shared_ptr<Transport> transport;
    std::string carrier = "carrier";
    axk::app::DeviceCandidate candidate() const {
        return {"candidate", "Synthetic disk", "disk-serial", disk->size, 512U, disk->safety.identity};
    }
    Result<std::vector<axk::app::DeviceCandidate>> list() override { return std::vector{candidate()}; }
    Result<axk::app::ConnectedDevice> connect(std::string_view, std::string_view, std::string_view) override {
        disk->writable = false;
        disk->claimed = false;
        transport = std::make_shared<Transport>(disk, carrier);
        return axk::app::ConnectedDevice{
            transport, candidate(), {{"disk", "Whole disk", 0U, disk->size, 512U, false, {}}}};
    }
};

class Fixture : public testing::Test {
  protected:
    std::shared_ptr<Provider> provider = std::make_shared<Provider>();
    std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("axk-format-" + *axk::app::secure_random_hex(8U));
    std::unique_ptr<axk::app::DeviceSessionManager> devices;
    std::unique_ptr<axk::app::DeviceFormatService> formats;
    std::chrono::steady_clock::time_point now{};
    axk::app::OperationContext context{};
    axk::app::DeviceSessionSummary session;

    void SetUp() override {
        context.owner_id = "owner";
        context.mutation_id = "format-operation";
        connect();
    }
    void TearDown() override {
        formats.reset();
        devices.reset();
        std::filesystem::remove_all(directory);
    }
    void connect() {
        formats.reset();
        devices.reset();
        devices = std::make_unique<axk::app::DeviceSessionManager>(directory, true, true, provider);
        formats = std::make_unique<axk::app::DeviceFormatService>(*devices, [&] { return now; });
        ASSERT_TRUE(devices->list(context.owner_id));
        const auto connected = devices->connect("candidate", context.owner_id);
        ASSERT_TRUE(connected) << connected.error().message;
        session = *connected;
    }
    Result<Json> inspect() {
        return formats->inspect({{"sessionId", session.session_id}, {"targetId", "disk"}}, context);
    }
    Result<Json> review() {
        const auto inspected = inspect();
        if (!inspected)
            return std::unexpected(inspected.error());
        return formats->plan({{"sessionId", session.session_id},
                              {"inspectionToken", inspected->at("inspectionToken")},
                              {"partitionCount", 1}},
                             context);
    }
    Json consent(const Json &plan) const {
        return {{"sessionId", session.session_id},
                {"planToken", plan.at("planToken")},
                {"confirmationText", plan.at("inspection").at("confirmationText")},
                {"exclusiveUseAcknowledged", true},
                {"dataLossAcknowledged", true}};
    }
};
} // namespace device_format_test
