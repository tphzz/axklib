#include "route_registration.hpp"

#include <string_view>
#include <utility>

namespace axk::server::detail {
namespace {

constexpr std::string_view event_subprotocol{"axklib.events.v1"};

} // namespace

void register_event_routes(ServerCrowApp &app, EventRoutes routes) {
    app.route_dynamic("/api/v1/jobs/<string>")
        .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Delete)(std::move(routes.job));
    app.route_dynamic("/api/v1/jobs/<string>/events")(std::move(routes.job_events));
    app.route_dynamic("/api/v1/event-tickets").methods(crow::HTTPMethod::Post)(std::move(routes.event_ticket));

    CROW_WEBSOCKET_ROUTE(app, "/api/v1/events")
        .subprotocols({std::string{event_subprotocol}})
        .max_payload(routes.maximum_websocket_payload_bytes)
        .onaccept(std::move(routes.websocket_accept))
        .onopen(std::move(routes.websocket_open))
        .onmessage(std::move(routes.websocket_message))
        .onclose(std::move(routes.websocket_close));
}

} // namespace axk::server::detail
