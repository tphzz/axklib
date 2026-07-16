#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include <nlohmann/json.hpp>

#include "axklib/application/operation_registry.hpp"
#include "axklib/server/config.hpp"
#include "axklib/server/server.hpp"

int main(int argc, char **argv) {
    const auto command_line = axk::server::parse_command_line(argc, argv);
    if (!command_line) {
        std::cerr << command_line.error().message << '\n';
        return 2;
    }

    auto registry = axk::app::make_operation_registry();
    for (auto descriptor : {
             axk::app::OperationDescriptor{"fixture.echo.alpha",
                                           {},
                                           axk::app::HttpMethod::post,
                                           "/api/v1/fixture-operations",
                                           axk::app::ExecutionMode::request,
                                           "ALPHA",
                                           "ExtractionRequest",
                                           "JsonObject",
                                           axk::app::OperationClass::read,
                                           false,
                                           false},
             axk::app::OperationDescriptor{"fixture.echo.beta",
                                           {},
                                           axk::app::HttpMethod::post,
                                           "/api/v1/fixture-operations",
                                           axk::app::ExecutionMode::request,
                                           "BETA",
                                           "ExtractionRequest",
                                           "JsonObject",
                                           axk::app::OperationClass::read,
                                           false,
                                           false},
             axk::app::OperationDescriptor{"fixture.job",
                                           {},
                                           axk::app::HttpMethod::post,
                                           "/api/v1/fixture-jobs",
                                           axk::app::ExecutionMode::job,
                                           {},
                                           "JsonObject",
                                           "JsonObject",
                                           axk::app::OperationClass::read,
                                           false,
                                           false},
         }) {
        if (const auto declared = registry.declare(std::move(descriptor)); !declared) {
            std::cerr << declared.error().message << '\n';
            return 2;
        }
    }
    const auto echo_invocations = std::make_shared<std::atomic<std::uint64_t>>(0U);
    for (const auto operation_id : {"fixture.echo.alpha", "fixture.echo.beta"}) {
        if (const auto echo = registry.bind(
                operation_id,
                [operation_id, echo_invocations](const nlohmann::json &request, const axk::app::OperationContext &) {
                    const auto invocation = echo_invocations->fetch_add(1U) + 1U;
                    return axk::app::Result<nlohmann::json>{nlohmann::json{
                        {"operationId", operation_id}, {"invocationCount", invocation}, {"echo", request}}};
                });
            !echo) {
            std::cerr << echo.error().message << '\n';
            return 2;
        }
    }
    const auto bound =
        registry.bind("fixture.job", [](const nlohmann::json &request, const axk::app::OperationContext &context) {
            for (std::uint64_t completed = 1U; completed <= 3U; ++completed) {
                if (context.cancellation.is_cancelled()) {
                    return axk::app::Result<nlohmann::json>{
                        std::unexpected(axk::app::Error{"cancelled", "fixture operation was cancelled"})};
                }
                if (context.progress != nullptr) {
                    context.progress->report(
                        {axk::ProgressPhase::reading, completed, 3U, "fixture progress", std::nullopt});
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
            return axk::app::Result<nlohmann::json>{nlohmann::json{{"fixture", true}, {"request", request}}};
        });
    if (!bound) {
        std::cerr << bound.error().message << '\n';
        return 2;
    }
    const auto result = axk::server::run(command_line->config, std::move(registry));
    if (!result) {
        std::cerr << result.error().message << '\n';
        return 2;
    }
    return *result;
}
