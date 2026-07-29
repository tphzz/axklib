#include "server_application.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "authentication.hpp"
#include "axklib/server/job_json.hpp"
#include "axklib/server/telemetry.hpp"
#include "axklib/utf8.hpp"
#include "axklib/writer.hpp"
#include "download_reader.hpp"
#include "http_headers.hpp"
#include "route_registration.hpp"
#include "server_support.hpp"

namespace axk::server::detail {

axk::app::Result<Json> ServerApplication::wire_job_snapshot(const axk::app::JobSnapshot &snapshot) const {
    auto result = axk::server::job_snapshot_json(snapshot);
    if (snapshot.result) {
        const auto *descriptor = registry_.find(snapshot.operation_id);
        if (descriptor == nullptr)
            return std::unexpected(axk::app::Error{"response_contract_error", "job operation metadata is missing"});
        auto wire_result = openapi_validator_.wire_value(descriptor->result_schema, *snapshot.result);
        if (const auto valid = openapi_validator_.validate(descriptor->result_schema, wire_result); !valid) {
            return std::unexpected(
                axk::app::Error{"response_contract_error", "job result violated its declared schema"});
        }
        result["result"] = std::move(wire_result);
    }
    if (const auto valid = openapi_validator_.validate("Job", result); !valid)
        return std::unexpected(axk::app::Error{"response_contract_error", "job snapshot violated its schema"});
    return result;
}

crow::response ServerApplication::operation_response(const crow::request &request,
                                                     const std::vector<std::string> &operation_ids) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    Json input = Json::object();
    if (!request.body.empty()) {
        auto parsed = parse_json_body(request, config_);
        if (!parsed)
            return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);
        input = std::move(*parsed);
    }

    std::string selected;
    if (operation_ids.size() == 1U) {
        selected = operation_ids.front();
    } else {
        const auto operation = input.find("operationId");
        if (operation == input.end() || !operation->is_string()) {
            return error_response(400, {"operation_id_required", "operationId selects an operation on this route"}, id);
        }
        selected = operation->get<std::string>();
        input.erase(operation);
        if (std::ranges::find(operation_ids, selected) == operation_ids.end())
            return error_response(400, {"invalid_operation_id", "operationId is not valid for this route"}, id);
    }

    const auto descriptor = registry_.find(selected);
    if (descriptor == nullptr)
        return error_response(500, {"contract_error", "operation metadata is not available"}, id);
    if (const auto valid = openapi_validator_.validate(descriptor->request_schema, input); !valid)
        return error_response(400, valid.error(), id);
    input = openapi_validator_.application_value(descriptor->request_schema, input);
    if (descriptor->mode == axk::app::ExecutionMode::job) {
        std::optional<std::string> idempotency_key;
        const auto supplied_key = request.get_header_value("Idempotency-Key");
        if (!supplied_key.empty())
            idempotency_key = supplied_key;
        const auto submitted = jobs_.submit(selected, std::move(input),
                                            {.owner_id = request_owner(request),
                                             .request_id = id,
                                             .cancellation = {},
                                             .progress = nullptr,
                                             .display_path = {},
                                             .diagnostic = operation_diagnostic_sink()},
                                            std::move(idempotency_key));
        if (!submitted)
            return error_response(status_for_error(submitted.error()), submitted.error(), id);
        audit(id, "job_submit", "allowed", request_owner(request), "job", submitted->job_id);
        auto snapshot = wire_job_snapshot(*submitted);
        if (!snapshot)
            return error_response(500, snapshot.error(), id);
        auto response = json_response(202, {{"data", std::move(*snapshot)}, {"meta", {{"requestId", id}}}}, id);
        response.set_header("Location", "/api/v1/jobs/" + submitted->job_id);
        return response;
    }
    const axk::app::OperationContext context{.owner_id = request_owner(request),
                                             .request_id = id,
                                             .cancellation = {},
                                             .progress = nullptr,
                                             .display_path = {},
                                             .diagnostic = operation_diagnostic_sink()};
    const auto accesses = registry_.path_accesses(selected, input, context);
    if (!accesses)
        return error_response(status_for_error(accesses.error(), 400), accesses.error(), id);
    auto reservation = path_reservations_.try_acquire(*accesses);
    if (!reservation)
        return error_response(409, reservation.error(), id);
    const auto result = registry_.invoke(selected, input, context);
    if (!result) {
        return error_response(status_for_error(result.error()), result.error(), id);
    }
    auto wire_result = openapi_validator_.wire_value(descriptor->result_schema, *result);
    if (const auto valid = openapi_validator_.validate(descriptor->result_schema, wire_result); !valid)
        return error_response(500, {"response_contract_error", "operation result violated its declared schema"}, id);
    return json_response(200, {{"data", std::move(wire_result)}, {"meta", {{"requestId", id}}}}, id);
}

crow::response ServerApplication::job_response(const crow::request &request, const std::string &job_id) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    if (request.method == crow::HTTPMethod::Delete) {
        if (const auto cancelled = jobs_.cancel(job_id, request_owner(request)); !cancelled) {
            audit(id, "job_cancel", "denied", request_owner(request), "job", job_id);
            return error_response(status_for_error(cancelled.error()), cancelled.error(), id);
        }
        audit(id, "job_cancel", "allowed", request_owner(request), "job", job_id);
    }
    const auto snapshot = jobs_.status(job_id, request_owner(request));
    if (!snapshot)
        return error_response(status_for_error(snapshot.error()), snapshot.error(), id);
    auto wire_snapshot = wire_job_snapshot(*snapshot);
    if (!wire_snapshot)
        return error_response(500, wire_snapshot.error(), id);
    return json_response(200, {{"data", std::move(*wire_snapshot)}, {"meta", {{"requestId", id}}}}, id);
}

crow::response ServerApplication::job_events_response(const crow::request &request, const std::string &job_id) const {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto after_sequence = parse_sequence(request.url_params.get("afterSequence"));
    if (!after_sequence)
        return error_response(400, {"invalid_cursor", "afterSequence must be an unsigned integer"}, id);
    const auto replay = jobs_.replay(job_id, request_owner(request), *after_sequence);
    if (!replay)
        return error_response(status_for_error(replay.error()), replay.error(), id);
    Json events = Json::array();
    for (const auto &event : *replay)
        events.push_back(axk::server::job_event_json(event));
    return json_response(
        200,
        {{"data", {{"events", std::move(events)}}}, {"meta", {{"requestId", id}, {"afterSequence", *after_sequence}}}},
        id);
}

crow::response ServerApplication::event_ticket_response(const crow::request &request) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto ticket = event_tickets_.issue(request_owner(request));
    if (!ticket)
        return error_response(status_for_error(ticket.error()), ticket.error(), id);
    return json_response(201,
                         {{"data",
                           {{"ticket", ticket->ticket_id},
                            {"expiresInSeconds", ticket->expires_in_seconds},
                            {"websocketUrl", "/api/v1/events"},
                            {"subprotocol", event_subprotocol}}},
                          {"meta", {{"requestId", id}}}},
                         id);
}

void ServerApplication::broadcast(const axk::app::JobEvent &event) {
    const auto message = axk::server::job_event_json(event).dump();
    std::vector<EventClientHandle> clients;
    {
        const std::scoped_lock lock{event_clients_mutex_};
        clients = event_clients_;
    }
    for (const auto &client : clients) {
        const std::scoped_lock lock{client->mutex};
        if (client->owner_id != event.owner_id || client->connection == nullptr)
            continue;
        if (!client->delivery_budget.admit(message.size())) {
            auto *connection = client->connection;
            client->connection = nullptr;
            websocket_clients_evicted_.fetch_add(1U, std::memory_order_relaxed);
            connection->close("event delivery budget exhausted; reconnect and replay", 1013U);
            continue;
        }
        client->connection->send_text(message);
    }
}

} // namespace axk::server::detail
