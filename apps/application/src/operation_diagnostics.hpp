#pragma once
#include "axklib/application/operation_registry.hpp"
#include "axklib/sdk/diagnostics.hpp"
#include <chrono>
#include <string_view>
namespace axk::app {
class OperationDiagnosticSink final : public diagnostic_sink {
  public:
    explicit OperationDiagnosticSink(const OperationContext &context) : context_(context) {}
    bool enabled(diagnostic_level level) const noexcept override {
        try {
            return context_.diagnostic && (!context_.diagnostic_enabled || context_.diagnostic_enabled(level));
        } catch (...) {
            return false;
        }
    }
    void report(const diagnostic_event &event) override {
        try {
            if (!enabled(event.level))
                return;
            const auto source = event.source == diagnostic_source::filesystem ? "filesystem"
                                : event.source == diagnostic_source::devices  ? "devices"
                                : event.source == diagnostic_source::audio    ? "audio"
                                                                              : "objects";
            const auto level = event.level == diagnostic_level::trace     ? "trace"
                               : event.level == diagnostic_level::debug   ? "debug"
                               : event.level == diagnostic_level::warning ? "warning"
                               : event.level == diagnostic_level::error   ? "error"
                                                                          : "info";
            nlohmann::json value{{"event", event.event},
                                 {"source", source},
                                 {"level", level},
                                 {"phase", event.phase},
                                 {"outcome", event.outcome},
                                 {"durationMs", static_cast<double>(event.duration_us) / 1000.0},
                                 {"requestId", context_.request_id}};
            for (const auto &metric : event.metrics)
                value[metric.name] = metric.value;
            context_.diagnostic(value);
        } catch (...) {
        }
    }

  private:
    const OperationContext &context_;
};
Result<nlohmann::json> invoke_diagnostic_job(const OperationRegistry &registry, std::string_view operation_id,
                                             const nlohmann::json &request, OperationContext context,
                                             std::string_view job_id);
} // namespace axk::app
