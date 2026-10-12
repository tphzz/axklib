#pragma once

#include <chrono>
#include <cstdint>

namespace axk::app::device_detail {

class IoTiming {
  public:
    IoTiming(std::uint64_t &calls, std::uint64_t &nanoseconds) : nanoseconds_(nanoseconds) { ++calls; }
    ~IoTiming() {
        nanoseconds_ += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started_).count());
    }
    IoTiming(const IoTiming &) = delete;
    IoTiming &operator=(const IoTiming &) = delete;

  private:
    std::uint64_t &nanoseconds_;
    std::chrono::steady_clock::time_point started_{std::chrono::steady_clock::now()};
};

} // namespace axk::app::device_detail
