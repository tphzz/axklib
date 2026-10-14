#include "server_application.hpp"

#include <string>
#include <utility>

#include "server_support.hpp"

namespace axk::server::detail {

crow::response ServerApplication::conversion_response(const crow::request &request, const std::string &plan_id,
                                                      bool execute) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto owner = request_owner(request);
    if (execute) {
        const auto supplied_key = request.get_header_value("Idempotency-Key");
        if (supplied_key.empty())
            return error_response(400, {"idempotency_key_required", "operation requires an idempotency key"}, id);
        if (supplied_key.size() > 128U)
            return error_response(400, {"invalid_idempotency_key", "idempotency key must contain 1 to 128 bytes"}, id);
        auto input = parse_validated_json_body(request, "ConversionExecutionRequest");
        if (!input)
            return error_response(status_for_error(input.error(), 400), input.error(), id);
        app::OperationContext context{};
        context.owner_id = owner;
        context.request_id = id;
        context.diagnostic = operation_diagnostic_sink();
        context.diagnostic_enabled = [this](diagnostic_level level) { return diagnostics_.enabled(level); };
        auto submitted = conversions_.execute(plan_id, *input, std::move(context));
        if (!submitted)
            return error_response(status_for_error(submitted.error()), submitted.error(), id);
        auto snapshot = wire_job_snapshot(*submitted);
        if (!snapshot)
            return error_response(500, snapshot.error(), id);
        audit(id, "job_submit", "allowed", owner, "job", submitted->job_id);
        auto response = json_response(202, {{"data", std::move(*snapshot)}, {"meta", {{"requestId", id}}}}, id);
        response.set_header("Location", "/api/v1/jobs/" + submitted->job_id);
        return response;
    }
    if (request.method == crow::HTTPMethod::Delete) {
        auto released = conversions_.release(plan_id, owner);
        if (!released)
            return error_response(status_for_error(released.error()), released.error(), id);
        return json_response(200, {{"data", {{"released", true}}}, {"meta", {{"requestId", id}}}}, id);
    }
    std::size_t offset{}, limit{200U};
    for (const auto &[name, destination] : {std::pair{"offset", &offset}, std::pair{"limit", &limit}}) {
        if (const auto *text = request.url_params.get(name)) {
            const auto parsed = parse_unsigned(text);
            if (!parsed || *parsed > 32768U)
                return error_response(400, {"invalid_request", "Conversion page parameter is out of range"}, id);
            *destination = static_cast<std::size_t>(*parsed);
        }
    }
    auto value =
        plan_id.empty() ? app::Result<Json>{conversions_.profiles()} : conversions_.plan(plan_id, owner, offset, limit);
    if (!value)
        return error_response(status_for_error(value.error()), value.error(), id);
    const auto schema = plan_id.empty() ? "ConversionProfiles" : "ConversionPlan";
    auto wire = openapi_validator_.wire_value(schema, *value);
    if (auto valid = openapi_validator_.validate(schema, wire); !valid)
        return error_response(500, {"response_contract_error", "Conversion response violates its schema"}, id);
    return json_response(200, {{"data", std::move(wire)}, {"meta", {{"requestId", id}}}}, id);
}

} // namespace axk::server::detail
