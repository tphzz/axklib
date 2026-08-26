#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <crow.h>

#include "axklib/application/operation_registry.hpp"
#include "transport.hpp"

namespace axk::server::detail {

using RequestRoute = std::function<crow::response(const crow::request &)>;
using ItemRoute = std::function<crow::response(const crow::request &, std::string)>;
using ResponseRoute = std::function<void(const crow::request &, crow::response &)>;
using ItemResponseRoute = std::function<void(const crow::request &, crow::response &, std::string)>;

struct InfrastructureRoutes {
    RequestRoute health_ready;
    RequestRoute capabilities;
    RequestRoute metrics;
    RequestRoute shutdown;
    RequestRoute roots;
    RequestRoute workspaces;
    RequestRoute workspace_reset;
    ItemRoute workspace_item;
    RequestRoute host_directory_roots;
    RequestRoute host_directory_list;
    RequestRoute openapi;
};

void register_infrastructure_routes(ServerCrowApp &app, InfrastructureRoutes routes);

struct FileRoutes {
    RequestRoute directory_list;
    RequestRoute media_source_inspect;
    RequestRoute metadata;
    RequestRoute create_directory;
    RequestRoute mutate_entry;
    ResponseRoute file_content;
    ItemResponseRoute download_archive_content;
    ItemRoute image;
    ItemRoute attach_companions;
    ItemRoute image_content;
    ItemRoute image_objects;
    ItemRoute image_relationships;
    ItemRoute image_system_program_contexts;
    ItemRoute image_allocation_map;
    ItemRoute image_validation;
    ItemRoute image_preview;
    ItemRoute audition_content;
    ItemRoute delete_audition;
    RequestRoute uploads;
    ItemRoute upload;
    ItemRoute complete_upload;
    ItemRoute materialize_upload;
};

void register_file_routes(ServerCrowApp &app, FileRoutes routes);

struct EventRoutes {
    ItemRoute job;
    ItemRoute job_events;
    RequestRoute event_ticket;
    std::size_t maximum_websocket_payload_bytes{};
    std::function<void(const crow::request &, std::optional<crow::response> &, void **)> websocket_accept;
    std::function<void(crow::websocket::connection &)> websocket_open;
    std::function<void(crow::websocket::connection &, const std::string &, bool)> websocket_message;
    std::function<void(crow::websocket::connection &, const std::string &, std::uint16_t)> websocket_close;
};

void register_event_routes(ServerCrowApp &app, EventRoutes routes);

using OperationRouteHandler =
    std::function<crow::response(const crow::request &, const std::vector<std::string> &operation_ids)>;

void register_operation_routes(ServerCrowApp &app, const app::OperationRegistry &registry,
                               OperationRouteHandler handler);

} // namespace axk::server::detail
