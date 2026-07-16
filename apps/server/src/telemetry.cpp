#include "axklib/server/telemetry.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

void axk::server::RequestTelemetry::begin_request() noexcept {
    total_requests_.fetch_add(1U, std::memory_order_relaxed);
    active_requests_.fetch_add(1U, std::memory_order_relaxed);
}

void axk::server::RequestTelemetry::complete_request(int status, std::chrono::milliseconds duration) noexcept {
    active_requests_.fetch_sub(1U, std::memory_order_relaxed);
    if (status >= 200 && status < 300)
        responses_2xx_.fetch_add(1U, std::memory_order_relaxed);
    else if (status >= 400 && status < 500)
        responses_4xx_.fetch_add(1U, std::memory_order_relaxed);
    else if (status >= 500)
        responses_5xx_.fetch_add(1U, std::memory_order_relaxed);
    total_duration_ms_.fetch_add(static_cast<std::uint64_t>(std::max<std::int64_t>(duration.count(), 0)),
                                 std::memory_order_relaxed);
}

axk::server::RequestTelemetrySnapshot axk::server::RequestTelemetry::snapshot() const noexcept {
    return {.total_requests = total_requests_.load(std::memory_order_relaxed),
            .active_requests = active_requests_.load(std::memory_order_relaxed),
            .responses_2xx = responses_2xx_.load(std::memory_order_relaxed),
            .responses_4xx = responses_4xx_.load(std::memory_order_relaxed),
            .responses_5xx = responses_5xx_.load(std::memory_order_relaxed),
            .total_duration_ms = total_duration_ms_.load(std::memory_order_relaxed)};
}

std::string axk::server::structured_request_log(std::string_view request_id, std::string_view method,
                                                std::string_view path, int status, std::chrono::milliseconds duration) {
    const auto query = path.find('?');
    const auto bounded_path = path.substr(0U, std::min(query, std::size_t{256U}));
    return nlohmann::json{{"event", "http_request"},
                          {"requestId", request_id.substr(0U, 96U)},
                          {"method", method.substr(0U, 16U)},
                          {"path", bounded_path},
                          {"status", status},
                          {"durationMs", std::max<std::int64_t>(duration.count(), 0)}}
        .dump();
}

std::string axk::server::structured_audit_log(std::string_view request_id, std::string_view action,
                                              std::string_view outcome, std::string_view principal_id,
                                              std::string_view resource_type, std::string_view resource_id) {
    auto document = nlohmann::json{{"event", "security_audit"},
                                   {"requestId", request_id.substr(0U, 96U)},
                                   {"action", action.substr(0U, 64U)},
                                   {"outcome", outcome.substr(0U, 16U)}};
    if (!principal_id.empty())
        document["principalId"] = principal_id.substr(0U, 64U);
    if (!resource_type.empty())
        document["resourceType"] = resource_type.substr(0U, 32U);
    if (!resource_id.empty())
        document["resourceId"] = resource_id.substr(0U, 96U);
    return document.dump();
}
