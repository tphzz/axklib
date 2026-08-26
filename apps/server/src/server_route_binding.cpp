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

void ServerApplication::register_infrastructure_routes() {
    axk::server::detail::register_infrastructure_routes(
        app_, {.health_ready = [this](const crow::request &request) { return health_ready_response(request); },
               .capabilities = [this](const crow::request &request) { return capability_response(request); },
               .metrics = [this](const crow::request &request) { return metrics_response(request); },
               .shutdown = [this](const crow::request &request) { return shutdown_response(request); },
               .roots = [this](const crow::request &request) { return roots_response(request); },
               .workspaces =
                   [this](const crow::request &request) {
                       return request.method == crow::HTTPMethod::Get ? workspace_snapshot_response(request)
                                                                      : workspace_create_response(request);
                   },
               .workspace_reset = [this](const crow::request &request) { return workspace_reset_response(request); },
               .workspace_item =
                   [this](const crow::request &request, const std::string &workspace_id) {
                       return workspace_item_response(request, workspace_id);
                   },
               .host_directory_roots =
                   [this](const crow::request &request) { return host_directory_roots_response(request); },
               .host_directory_list =
                   [this](const crow::request &request) { return host_directory_listing_response(request); },
               .openapi = [this](const crow::request &request) { return openapi_response(request); }});

    axk::server::detail::register_file_routes(
        app_,
        {.directory_list = [this](const crow::request &request) { return directory_listing_response(request); },
         .media_source_inspect =
             [this](const crow::request &request) { return media_source_inspection_response(request); },
         .metadata = [this](const crow::request &request) { return metadata_response(request); },
         .create_directory = [this](const crow::request &request) { return create_directory_response(request); },
         .mutate_entry =
             [this](const crow::request &request) {
                 return request.method == crow::HTTPMethod::Patch ? rename_entry_response(request)
                                                                  : delete_entry_response(request);
             },
         .file_content =
             [this](const crow::request &request, crow::response &response) {
                 response = download_response(request);
                 if (request.method == crow::HTTPMethod::HEAD && response.code == 200) {
                     response.skip_body = false;
                     response.manual_length_header = true;
                 }
                 response.end();
             },
         .download_archive_content =
             [this](const crow::request &request, crow::response &response, const std::string &archive_id) {
                 download_archive_response(request, response, archive_id);
             },
         .image = [this](const crow::request &request,
                         const std::string &image_id) { return image_response(request, image_id); },
         .attach_companions =
             [this](const crow::request &request, const std::string &image_id) {
                 return attach_companions_response(request, image_id);
             },
         .image_content = [this](const crow::request &request,
                                 const std::string &image_id) { return image_content_response(request, image_id); },
         .image_objects = [this](const crow::request &request,
                                 const std::string &image_id) { return image_objects_response(request, image_id); },
         .image_relationships =
             [this](const crow::request &request, const std::string &image_id) {
                 return image_relationships_response(request, image_id);
             },
         .image_system_program_contexts =
             [this](const crow::request &request, const std::string &image_id) {
                 return image_system_program_contexts_response(request, image_id);
             },
         .image_allocation_map =
             [this](const crow::request &request, const std::string &image_id) {
                 return image_allocation_map_response(request, image_id);
             },
         .image_validation =
             [this](const crow::request &request, const std::string &image_id) {
                 return image_validation_response(request, image_id);
             },
         .image_preview = [this](const crow::request &request,
                                 const std::string &image_id) { return image_preview_response(request, image_id); },
         .audition_content =
             [this](const crow::request &request, const std::string &audition_id) {
                 return audition_content_response(request, audition_id);
             },
         .delete_audition =
             [this](const crow::request &request, const std::string &audition_id) {
                 return audition_delete_response(request, audition_id);
             },
         .uploads =
             [this](const crow::request &request) {
                 return request.method == crow::HTTPMethod::Options ? preflight_response(request)
                                                                    : create_upload_response(request);
             },
         .upload = [this](const crow::request &request,
                          const std::string &upload_id) { return upload_response(request, upload_id); },
         .complete_upload =
             [this](const crow::request &request, const std::string &upload_id) {
                 return complete_upload_response(request, upload_id);
             },
         .materialize_upload =
             [this](const crow::request &request, const std::string &upload_id) {
                 return materialize_upload_response(request, upload_id);
             }});
}

void ServerApplication::register_event_route() {
    axk::server::detail::register_event_routes(
        app_, {.job = [this](const crow::request &request,
                             const std::string &job_id) { return job_response(request, job_id); },
               .job_events = [this](const crow::request &request,
                                    const std::string &job_id) { return job_events_response(request, job_id); },
               .event_ticket = [this](const crow::request &request) { return event_ticket_response(request); },
               .maximum_websocket_payload_bytes = config_.maximum_websocket_payload_bytes,
               .websocket_accept =
                   [this](const crow::request &request, std::optional<crow::response> &response, void **user_data) {
                       const auto id = request_id(request);
                       if (!origin_allowed(request)) {
                           response = error_response(403, {"origin_denied", "request origin is not allowed"}, id);
                           return;
                       }
                       if (!requests_subprotocol(request, event_subprotocol)) {
                           response = error_response(
                               400, {"websocket_subprotocol_required", "axklib.events.v1 subprotocol is required"}, id);
                           return;
                       }
                       const auto *ticket_id = request.url_params.get("ticket");
                       if (ticket_id == nullptr || *ticket_id == '\0') {
                           response = error_response(401, {"event_ticket_required", "event ticket is required"}, id);
                           return;
                       }
                       auto owner_id = event_tickets_.consume(ticket_id);
                       if (!owner_id) {
                           response = error_response(401, owner_id.error(), id);
                           return;
                       }
                       auto client = std::make_shared<EventClient>(config_.maximum_websocket_delivery_events,
                                                                   config_.maximum_websocket_delivery_bytes);
                       client->owner_id = std::move(*owner_id);
                       *user_data = new EventClientHandle{std::move(client)};
                   },
               .websocket_open =
                   [this](crow::websocket::connection &connection) {
                       auto *holder = static_cast<EventClientHandle *>(connection.userdata());
                       if (holder == nullptr || !*holder) {
                           connection.close("missing event client", crow::websocket::UnexpectedCondition);
                           return;
                       }
                       {
                           const std::scoped_lock lock{(*holder)->mutex};
                           (*holder)->connection = &connection;
                       }
                       const std::scoped_lock lock{event_clients_mutex_};
                       event_clients_.push_back(*holder);
                   },
               .websocket_message =
                   [](crow::websocket::connection &connection, const std::string &, bool) {
                       connection.close("client messages are not accepted", crow::websocket::PolicyViolated);
                   },
               .websocket_close =
                   [this](crow::websocket::connection &connection, const std::string &, std::uint16_t) {
                       auto *holder = static_cast<EventClientHandle *>(connection.userdata());
                       if (holder == nullptr)
                           return;
                       const auto client = *holder;
                       if (client) {
                           {
                               const std::scoped_lock lock{client->mutex};
                               client->connection = nullptr;
                           }
                           const std::scoped_lock lock{event_clients_mutex_};
                           std::erase(event_clients_, client);
                       }
                       delete holder;
                       connection.userdata(nullptr);
                   }});
}

void ServerApplication::register_operation_routes() {
    axk::server::detail::register_operation_routes(
        app_, registry_, [this](const crow::request &request, const std::vector<std::string> &operation_ids) {
            return operation_response(request, operation_ids);
        });
}

} // namespace axk::server::detail
