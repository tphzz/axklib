#include "route_registration.hpp"

#include <utility>

namespace axk::server::detail {

void register_infrastructure_routes(ServerCrowApp &app, InfrastructureRoutes routes) {
    app.route_dynamic("/api/v1/system/health/live")([] { return crow::response{204}; });
    app.route_dynamic("/api/v1/system/health/ready")(std::move(routes.health_ready));
    app.route_dynamic("/api/v1/system/capabilities")(std::move(routes.capabilities));
    app.route_dynamic("/api/v1/system/metrics")(std::move(routes.metrics));
    app.route_dynamic("/api/v1/system/shutdown").methods(crow::HTTPMethod::Post)(std::move(routes.shutdown));
    app.route_dynamic("/api/v1/roots")(std::move(routes.roots));
    app.route_dynamic("/api/v1/workspaces")
        .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post)(std::move(routes.workspaces));
    app.route_dynamic("/api/v1/workspaces/recovery/reset")
        .methods(crow::HTTPMethod::Post)(std::move(routes.workspace_reset));
    app.route_dynamic("/api/v1/workspaces/<string>")
        .methods(crow::HTTPMethod::Patch, crow::HTTPMethod::Delete)(std::move(routes.workspace_item));
    app.route_dynamic("/api/v1/host-directories/roots")(std::move(routes.host_directory_roots));
    app.route_dynamic("/api/v1/host-directories/list")
        .methods(crow::HTTPMethod::Post)(std::move(routes.host_directory_list));
    app.route_dynamic("/api/v1/openapi.json")(std::move(routes.openapi));
}

} // namespace axk::server::detail
