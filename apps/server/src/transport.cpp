#include "transport.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <iostream>
#include <mutex>
#include <ranges>

namespace axk::server::detail {
namespace {

std::string next_request_id() {
    static std::atomic<std::uint64_t> sequence{1U};
    return "request-" + std::to_string(sequence.fetch_add(1U, std::memory_order_relaxed));
}

bool valid_request_id(std::string_view value) {
    return !value.empty() && value.size() <= 96U && std::ranges::all_of(value, [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '-' || character == '_' || character == '.';
    });
}

} // namespace

void write_structured_log(std::string line) {
    static std::mutex mutex;
    const std::scoped_lock lock{mutex};
    std::clog << line << '\n';
}

std::string request_id(const crow::request &request) {
    const auto supplied = request.get_header_value("X-Request-Id");
    return valid_request_id(supplied) ? supplied : next_request_id();
}

void CorsMiddleware::before_handle(crow::request &request, crow::response &response, context &) {
    if (request.method != crow::HTTPMethod::Options)
        return;
    const auto origin = request.get_header_value("Origin");
    response.code = !origin.empty() && std::ranges::find(allowed_origins, origin) != allowed_origins.end() ? 204 : 403;
    response.end();
}

void CorsMiddleware::after_handle(crow::request &request, crow::response &response, context &) {
    const auto origin = request.get_header_value("Origin");
    const auto allowed = !origin.empty() && std::ranges::find(allowed_origins, origin) != allowed_origins.end();
    if (request.method == crow::HTTPMethod::Options && !allowed) {
        response.code = 403;
        return;
    }
    if (!allowed)
        return;
    response.set_header("Access-Control-Allow-Origin", origin);
    response.set_header("Vary", "Origin, Access-Control-Request-Method, Access-Control-Request-Headers");
    response.set_header("Access-Control-Allow-Methods", "GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS");
    response.set_header("Access-Control-Allow-Headers",
                        "Authorization, Content-Type, Idempotency-Key, If-Match, Range, X-Request-Id, Upload-Offset");
    response.set_header("Access-Control-Expose-Headers",
                        "Accept-Ranges, Content-Length, Content-Range, ETag, Location, Upload-Offset, X-Request-Id");
    if (request.method == crow::HTTPMethod::Options)
        response.set_header("Access-Control-Max-Age", "600");
}

void RequestTelemetryMiddleware::before_handle(crow::request &, crow::response &, context &request_context) const {
    request_context.started = std::chrono::steady_clock::now();
    request_context.begun = true;
    if (telemetry != nullptr)
        telemetry->begin_request();
}

void RequestTelemetryMiddleware::after_handle(crow::request &request, crow::response &response,
                                              context &request_context) const {
    if (telemetry == nullptr)
        return;
    if (!request_context.begun) {
        request_context.started = std::chrono::steady_clock::now();
        request_context.begun = true;
        telemetry->begin_request();
    }
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                request_context.started);
    telemetry->complete_request(response.code, duration);
    auto id = response.get_header_value("X-Request-Id");
    if (id.empty()) {
        id = request_id(request);
        response.set_header("X-Request-Id", id);
    }
    write_structured_log(
        structured_request_log(id, crow::method_name(request.method), request.url, response.code, duration));
}

} // namespace axk::server::detail
