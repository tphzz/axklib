#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace axk::server {

struct RequestTelemetrySnapshot {
    std::uint64_t total_requests{};
    std::uint64_t active_requests{};
    std::uint64_t responses_2xx{};
    std::uint64_t responses_4xx{};
    std::uint64_t responses_5xx{};
    std::uint64_t total_duration_ms{};
};

class RequestTelemetry {
  public:
    void begin_request() noexcept;
    void complete_request(int status, std::chrono::milliseconds duration) noexcept;
    [[nodiscard]] RequestTelemetrySnapshot snapshot() const noexcept;

  private:
    std::atomic<std::uint64_t> total_requests_{};
    std::atomic<std::uint64_t> active_requests_{};
    std::atomic<std::uint64_t> responses_2xx_{};
    std::atomic<std::uint64_t> responses_4xx_{};
    std::atomic<std::uint64_t> responses_5xx_{};
    std::atomic<std::uint64_t> total_duration_ms_{};
};

[[nodiscard]] std::string structured_request_log(std::string_view request_id, std::string_view method,
                                                 std::string_view path, int status, std::chrono::milliseconds duration);
[[nodiscard]] std::string structured_audit_log(std::string_view request_id, std::string_view action,
                                               std::string_view outcome, std::string_view principal_id = {},
                                               std::string_view resource_type = {}, std::string_view resource_id = {});

} // namespace axk::server
