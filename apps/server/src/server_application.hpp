#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <crow.h>
#include <nlohmann/json.hpp>

#include "archive_download_budget.hpp"
#include "axklib/application/alteration_journal.hpp"
#include "axklib/application/download_archives.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/jobs.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/application/uploads.hpp"
#include "axklib/server/config.hpp"
#include "axklib/server/contract.hpp"
#include "axklib/server/event_delivery_budget.hpp"
#include "axklib/server/event_dispatcher.hpp"
#include "axklib/server/event_tickets.hpp"
#include "axklib/server/request_validation.hpp"
#include "axklib/server/telemetry.hpp"
#include "axklib/server/workspaces.hpp"
#include "transport.hpp"

namespace axk::server::detail {

using Json = nlohmann::json;

class ServerApplication {
  public:
    ServerApplication(Config config, app::OperationRegistry registry, WorkspaceStore workspaces);
    ~ServerApplication();

    app::Result<int> run();

  private:
    static app::OperationRegistry prepare_registry(app::OperationRegistry registry, const app::Sandbox &sandbox,
                                                   app::UploadStore &uploads, app::ImageSessionManager &images,
                                                   app::AlterationJournalStore &journals,
                                                   app::DownloadArchiveStore &downloads,
                                                   const MediaBuildLimits &media_limits);

    struct EventClient {
        EventClient(std::size_t maximum_events, std::uint64_t maximum_bytes)
            : delivery_budget(maximum_events, maximum_bytes) {}

        std::mutex mutex;
        crow::websocket::connection *connection{};
        std::string owner_id;
        EventDeliveryBudget delivery_budget;
    };

    using EventClientHandle = std::shared_ptr<EventClient>;

    [[nodiscard]] std::filesystem::path upload_directory() const;
    [[nodiscard]] std::filesystem::path download_archive_directory() const;
    [[nodiscard]] std::filesystem::path alteration_journal_directory() const;

    std::optional<std::string> authenticated_principal(const crow::request &request) const;
    std::string request_owner(const crow::request &request) const;
    bool origin_allowed(const crow::request &request) const;
    void audit(std::string_view id, std::string_view action, std::string_view outcome,
               std::string_view principal_id = {}, std::string_view resource_type = {},
               std::string_view resource_id = {}) const;
    std::optional<crow::response> guard(const crow::request &request, std::string_view id) const;
    crow::response preflight_response(const crow::request &request) const;
    crow::response capability_response(const crow::request &request) const;
    const Json &openapi_document() const noexcept;
    app::Result<Json> parse_validated_json_body(const crow::request &request, std::string_view schema_name) const;

    crow::response roots_response(const crow::request &request);
    Json workspace_json(const WorkspaceInfo &workspace) const;
    crow::response workspace_snapshot_response(const crow::request &request);
    crow::response workspace_create_response(const crow::request &request);
    crow::response workspace_item_response(const crow::request &request, const std::string &workspace_id);
    crow::response workspace_reset_response(const crow::request &request);
    crow::response host_directory_roots_response(const crow::request &request) const;
    crow::response host_directory_listing_response(const crow::request &request) const;
    crow::response directory_listing_response(const crow::request &request) const;
    crow::response media_source_inspection_response(const crow::request &request) const;
    crow::response metadata_response(const crow::request &request) const;
    Json entry_metadata_json(const app::EntryMetadata &metadata) const;
    crow::response create_directory_response(const crow::request &request);
    crow::response rename_entry_response(const crow::request &request);
    crow::response delete_entry_response(const crow::request &request);

    Json image_summary_json(const app::ImageSessionSummary &summary) const;
    crow::response create_image_response(const crow::request &request);
    crow::response attach_companions_response(const crow::request &request, const std::string &image_id);
    crow::response image_response(const crow::request &request, const std::string &image_id);
    template <typename Item, typename Loader, typename Serializer>
    crow::response image_page_response(const crow::request &request, const std::string &image_id, Loader loader,
                                       Serializer serializer);
    crow::response image_content_response(const crow::request &request, const std::string &image_id);
    crow::response image_objects_response(const crow::request &request, const std::string &image_id);
    crow::response image_relationships_response(const crow::request &request, const std::string &image_id);
    crow::response image_system_program_contexts_response(const crow::request &request, const std::string &image_id);
    crow::response image_validation_response(const crow::request &request, const std::string &image_id);
    crow::response image_preview_response(const crow::request &request, const std::string &image_id);
    crow::response audition_content_response(const crow::request &request, const std::string &audition_id);
    crow::response audition_delete_response(const crow::request &request, const std::string &audition_id);

    crow::response create_upload_response(const crow::request &request);
    crow::response upload_response(const crow::request &request, const std::string &upload_id);
    crow::response complete_upload_response(const crow::request &request, const std::string &upload_id);
    crow::response materialize_upload_response(const crow::request &request, const std::string &upload_id);
    crow::response download_response(const crow::request &request);
    void download_archive_response(const crow::request &request, crow::response &response,
                                   const std::string &archive_id);

    app::Result<Json> wire_job_snapshot(const app::JobSnapshot &snapshot) const;
    crow::response operation_response(const crow::request &request, const std::vector<std::string> &operation_ids);
    crow::response job_response(const crow::request &request, const std::string &job_id);
    crow::response job_events_response(const crow::request &request, const std::string &job_id) const;
    crow::response event_ticket_response(const crow::request &request);
    void broadcast(const app::JobEvent &event);

    crow::response health_ready_response(const crow::request &request);
    crow::response metrics_response(const crow::request &request);
    crow::response shutdown_response(const crow::request &request);
    crow::response openapi_response(const crow::request &request);

    void register_infrastructure_routes();
    void register_event_route();
    void register_operation_routes();

    Config config_;
    WorkspaceStore workspaces_;
    app::Sandbox sandbox_;
    app::PathReservationCoordinator path_reservations_;
    app::UploadStore uploads_;
    app::DownloadArchiveStore download_archives_;
    ArchiveDownloadBudget archive_download_budget_;
    app::AlterationJournalStore alteration_journals_;
    app::ImageSessionManager images_;
    app::OperationRegistry registry_;
    Json openapi_document_;
    OpenApiValidator openapi_validator_;
    app::JobManager jobs_;
    EventTicketStore event_tickets_;
    EventDispatcher event_dispatcher_;
    RequestTelemetry request_telemetry_;
    std::mutex event_clients_mutex_;
    std::vector<EventClientHandle> event_clients_;
    app::JobManager::SubscriptionId job_subscription_{};
    ServerCrowApp app_;
    std::atomic_bool shutdown_requested_{};
    std::atomic<std::uint64_t> websocket_clients_evicted_{};
    bool state_storage_ready_{};
    bool startup_cleanup_ready_{};
};

} // namespace axk::server::detail
