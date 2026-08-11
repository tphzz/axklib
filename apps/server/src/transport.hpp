#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include <crow.h>

#include "axklib/server/telemetry.hpp"

namespace axk::server::detail {

void write_structured_log(std::string line);

[[nodiscard]] std::string request_id(const crow::request &request);

struct CorsMiddleware {
    struct context {};

    std::vector<std::string> allowed_origins;

    void before_handle(crow::request &request, crow::response &response, context &);
    void after_handle(crow::request &request, crow::response &response, context &);
};

struct RequestTelemetryMiddleware {
    struct context {
        std::chrono::steady_clock::time_point started;
        bool begun{};
    };

    RequestTelemetry *telemetry{};

    void before_handle(crow::request &, crow::response &, context &request_context) const;
    void after_handle(crow::request &request, crow::response &response, context &request_context) const;
};

using ServerCrowApp = crow::App<RequestTelemetryMiddleware, CorsMiddleware>;

} // namespace axk::server::detail
