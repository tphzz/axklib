#pragma once
#include <string_view>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "axklib/sdk/diagnostics.hpp"
#include <nlohmann/json.hpp>

namespace axk::server::detail {
void write_startup_error(std::string_view code, std::string_view message) noexcept;
class DiagnosticLog {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    using Writer = std::function<void(const std::string &)>;
    explicit DiagnosticLog(bool sidecar, Clock clock = std::chrono::steady_clock::now, Writer writer = {});
    ~DiagnosticLog();
    bool enabled(diagnostic_level level) const noexcept;
    diagnostic_level capture() const noexcept;
    void set_capture(diagnostic_level level) noexcept;
    void submit(nlohmann::json event) noexcept;
    void submit_line(const std::string &line) noexcept;

  private:
    void drain() noexcept;
    bool sidecar_{};
    std::uint8_t console_level_{};
    Clock clock_;
    Writer writer_;
    std::atomic<std::uint8_t> capture_level_{3};
    std::atomic<std::int64_t> expires_{};
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::string> queue_;
    std::size_t queued_bytes_{};
    std::uint64_t dropped_{};
    bool stopping_{};
    std::thread worker_;
};
} // namespace axk::server::detail
