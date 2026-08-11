#include "route_registration.hpp"

#include <map>
#include <utility>

namespace axk::server::detail {
namespace {

struct RouteKey {
    app::HttpMethod method;
    std::string route;

    friend bool operator<(const RouteKey &left, const RouteKey &right) {
        if (left.route != right.route)
            return left.route < right.route;
        return left.method < right.method;
    }
};

} // namespace

void register_operation_routes(ServerCrowApp &app, const app::OperationRegistry &registry,
                               OperationRouteHandler handler) {
    std::map<RouteKey, std::vector<std::string>> routes;
    for (const auto &entry : registry.entries())
        routes[{entry.descriptor.method, entry.descriptor.route}].push_back(entry.descriptor.id);

    for (auto &[key, operation_ids] : routes) {
        auto &route = app.route_dynamic(key.route);
        route.methods(key.method == app::HttpMethod::get ? crow::HTTPMethod::Get : crow::HTTPMethod::Post);
        route([handler, operation_ids = std::move(operation_ids)](const crow::request &request) {
            return handler(request, operation_ids);
        });
    }
}

} // namespace axk::server::detail
