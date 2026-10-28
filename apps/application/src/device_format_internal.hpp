#pragma once

#include <chrono>
#include <functional>

#include "axklib/writer.hpp"
#include "device_content_inspection.hpp"
#include "device_format.hpp"
#include "device_wire.hpp"

namespace axk::app::format_detail {
using Clock = std::chrono::steady_clock;
using namespace device_detail;
struct Inspection {
    std::string token;
    std::string owner;
    std::string session;
    DeviceTargetDescription target;
    DeviceWriteSafety safety;
    std::weak_ptr<Connection> connection;
    ContentInspection contents;
    Json wire;
    Clock::time_point expires;
};
struct Plan {
    std::string token;
    std::shared_ptr<Inspection> inspection;
    Guards guards;
    std::uint64_t size{};
    std::uint8_t partitions{};
    Clock::time_point expires;
    bool consumed{};
    bool revoked{};
};
struct Lease {
    std::shared_ptr<Connection> connection;
    std::shared_ptr<Plan> plan;
    std::string operation;
    std::filesystem::path directory;
    std::function<void()> release;
    ~Lease() {
        if (release)
            release();
    }
};

Result<void> require_safe(const DeviceWriteSafety &safety);
Result<void> require_fresh(const Connection &connection);
Result<void> check_initialized(std::shared_ptr<const RandomAccessReader> reader,
                               const HdsInitialization &initialization);
Result<Guards> read_initialization_guards(const Connection &connection, const DeviceTargetDescription &target,
                                          const HdsInitialization &initialization, Guards initial,
                                          const CancellationToken &cancellation);
} // namespace axk::app::format_detail

struct axk::app::DeviceFormatService::State {
    State(DeviceSessionManager &value, DeviceFormatService::Now clock) : devices(value), now(std::move(clock)) {}
    DeviceSessionManager &devices;
    DeviceFormatService::Now now;
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<format_detail::Inspection>, std::less<>> inspections;
    std::map<std::string, std::shared_ptr<format_detail::Plan>, std::less<>> plans;
    std::map<std::string, std::weak_ptr<format_detail::Lease>, std::less<>> pending;
    void prune() {
        std::erase_if(inspections, [&](const auto &entry) {
            return entry.second->connection.expired() || entry.second->expires <= now();
        });
        std::erase_if(plans, [&](const auto &entry) {
            return entry.second->inspection->connection.expired() || entry.second->expires <= now();
        });
        std::erase_if(pending, [](const auto &entry) { return entry.second.expired(); });
    }
};
