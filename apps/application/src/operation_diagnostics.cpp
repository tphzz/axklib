#include "operation_diagnostics.hpp"
#include <chrono>
#include <string>
#include <utility>
namespace axk::app {
Result<nlohmann::json> invoke_diagnostic_job(const OperationRegistry &registry, std::string_view operation_id,
                                             const nlohmann::json &request, OperationContext context,
                                             std::string_view job_id) {
    const auto destination = context.diagnostic;
    if (destination)
        context.diagnostic = [destination, job = std::string{job_id}, operation = std::string{operation_id},
                              request_id = context.request_id](const nlohmann::json &event) {
            try {
                auto value = event;
                value["jobId"] = job;
                value["operation"] = operation;
                value["requestId"] = request_id;
                destination(value);
            } catch (...) {
            }
        };
    const auto started = std::chrono::steady_clock::now();
    auto result = [&]() -> Result<nlohmann::json> {
        try {
            return registry.invoke(operation_id, request, context);
        } catch (...) {
            return std::unexpected(Error{"operation_exception", "operation raised an unexpected exception"});
        }
    }();
    if (context.diagnostic) {
        try {
            auto event = nlohmann::json{{"event", "operation_completed"},
                                        {"level", result ? "info" : "warning"},
                                        {"source", operation_id.starts_with("devices.") ? "devices" : "jobs"},
                                        {"outcome", result ? "success" : "failed"},
                                        {"durationMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                                                           std::chrono::steady_clock::now() - started)
                                                           .count()}};
            for (const auto *key : {"imageId", "objectId", "expectedRevision"})
                if (request.contains(key))
                    event[key] = request.at(key);
            if (!result) {
                event["code"] = result.error().code;
                if (result.error().code == "cancelled" || result.error().code == "operation_cancelled")
                    event["outcome"] = "cancelled";
            }
            context.diagnostic(event);
        } catch (...) {
        }
    }
    return result;
}
} // namespace axk::app
