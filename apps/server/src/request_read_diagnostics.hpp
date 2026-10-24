#pragma once

#include "diagnostic_log.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace axk::server::detail {
class RequestReadDiagnostics final : public diagnostic_sink {
  public:
    RequestReadDiagnostics(DiagnosticLog &log, std::string_view request, std::string_view image = {},
                           std::string_view document = {}, std::string_view object = {})
        : log_(log), request_(request), image_(image), document_(document), object_(object) {}
    bool enabled(diagnostic_level level) const noexcept override { return log_.enabled(level); }
    void report(const diagnostic_event &event) override {
        if (!enabled(event.level))
            return;
        nlohmann::json value{{"event", event.event},
                             {"source", "devices"},
                             {"level", "debug"},
                             {"requestId", request_},
                             {"phase", event.phase},
                             {"outcome", event.outcome},
                             {"durationMs", static_cast<double>(event.duration_us) / 1000.0}};
        if (!image_.empty())
            value["imageId"] = image_;
        if (!document_.empty())
            value["documentId"] = document_;
        if (!object_.empty())
            value["objectId"] = object_.substr(0U, 256U);
        for (const auto &metric : event.metrics)
            value[metric.name] = metric.value;
        log_.submit(std::move(value));
    }
    std::chrono::steady_clock::time_point serialization_start() const noexcept {
        return enabled(diagnostic_level::debug) ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
    }
    void serialized(std::chrono::steady_clock::time_point started) noexcept {
        if (started == std::chrono::steady_clock::time_point{})
            return;
        try {
            report({.level = diagnostic_level::debug,
                    .source = diagnostic_source::devices,
                    .event = "image_read_response",
                    .phase = "serialize",
                    .outcome = "success",
                    .duration_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                                  std::chrono::steady_clock::now() - started)
                                                                  .count()),
                    .metrics = {}});
        } catch (...) {
        }
    }

  private:
    DiagnosticLog &log_;
    std::string_view request_, image_, document_, object_;
};
} // namespace axk::server::detail
