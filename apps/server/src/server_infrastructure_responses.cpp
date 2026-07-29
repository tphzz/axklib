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

crow::response ServerApplication::health_ready_response(const crow::request &request) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto executor_ready = !shutdown_requested_.load(std::memory_order_relaxed);
    const auto upload_cleanup = uploads_.cleanup_snapshot();
    const auto startup_cleanup_ready = startup_cleanup_ready_ && cleanup_complete(upload_directory()) &&
                                       cleanup_complete(download_archive_directory());
    const auto state_storage_ready = state_storage_ready_ && alteration_journals_.storage_ready();
    const auto ready = state_storage_ready && startup_cleanup_ready && upload_cleanup.healthy && executor_ready;
    const auto workspace_snapshot = workspaces_.snapshot();
    const auto state = [](bool value) { return value ? "READY" : "NOT_READY"; };
    return json_response(
        ready ? 200 : 503,
        {{"data",
          {{"ready", ready},
           {"checks",
            {{"configuration", "READY"},
             {"sandbox", "READY"},
             {"workspaceConfiguration", axk::server::workspace_configuration_state_name(workspace_snapshot.state)},
             {"stateStorage", state(state_storage_ready)},
             {"startupCleanup", state(startup_cleanup_ready)},
             {"uploadCleanup", state(upload_cleanup.healthy)},
             {"executorAdmission", state(executor_ready)}}}}}},
        id);
}

crow::response ServerApplication::metrics_response(const crow::request &request) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto metrics = request_telemetry_.snapshot();
    const auto job_metrics = jobs_.metrics();
    const auto event_metrics = event_dispatcher_.snapshot();
    const auto upload_cleanup = uploads_.cleanup_snapshot();
    return json_response(200,
                         {{"data",
                           {{"totalRequests", metrics.total_requests},
                            {"activeRequests", metrics.active_requests},
                            {"responses2xx", metrics.responses_2xx},
                            {"responses4xx", metrics.responses_4xx},
                            {"responses5xx", metrics.responses_5xx},
                            {"totalDurationMs", metrics.total_duration_ms},
                            {"submittedJobs", job_metrics.submitted_jobs},
                            {"queuedJobs", job_metrics.queued_jobs},
                            {"runningJobs", job_metrics.running_jobs},
                            {"completedJobs", job_metrics.completed_jobs},
                            {"failedJobs", job_metrics.failed_jobs},
                            {"cancelledJobs", job_metrics.cancelled_jobs},
                            {"publishedJobEvents", job_metrics.published_events},
                            {"progressJobEvents", job_metrics.progress_events},
                            {"totalJobQueueWaitMs", job_metrics.total_queue_wait_ms},
                            {"totalJobExecutionMs", job_metrics.total_execution_ms},
                            {"totalJobPhaseDurationMs", job_metrics.total_phase_duration_ms},
                            {"totalJobCancellationLatencyMs", job_metrics.total_cancellation_latency_ms},
                            {"websocketEventsDelivered", event_metrics.delivered_events},
                            {"websocketEventsFailed", event_metrics.failed_events},
                            {"websocketEventsDropped", event_metrics.dropped_events},
                            {"websocketEventsPending", event_metrics.pending_events},
                            {"websocketClientsEvicted", websocket_clients_evicted_.load(std::memory_order_relaxed)},
                            {"uploadCleanupHealthy", upload_cleanup.healthy},
                            {"uploadCleanupFailedDeletions", upload_cleanup.failed_deletions},
                            {"uploadOrphanFiles", upload_cleanup.orphan_count},
                            {"uploadOrphanBytes", upload_cleanup.orphan_bytes},
                            {"uploadReservedBytes", upload_cleanup.reserved_bytes}}},
                          {"meta", {{"requestId", id}}}},
                         id);
}

crow::response ServerApplication::shutdown_response(const crow::request &request) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    if (config_.connection_file.empty()) {
        return error_response(404, {"route_not_found", "sidecar shutdown is not available in this deployment mode"},
                              id);
    }
    if (shutdown_requested_.exchange(true))
        return error_response(409, {"shutdown_in_progress", "server shutdown is already in progress"}, id);
    return json_response(202, {{"data", {{"accepted", true}}}}, id);
}

crow::response ServerApplication::openapi_response(const crow::request &request) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    crow::response response{200, "application/json", openapi_document().dump()};
    response.set_header("X-Request-Id", id);
    return response;
}

} // namespace axk::server::detail
