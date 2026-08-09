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

std::optional<std::string> ServerApplication::authenticated_principal(const crow::request &request) const {
    return axk::server::detail::authenticated_principal(config_, request);
}

std::string ServerApplication::request_owner(const crow::request &request) const {
    return *authenticated_principal(request);
}

bool ServerApplication::origin_allowed(const crow::request &request) const {
    return axk::server::detail::origin_allowed(config_, request);
}

void ServerApplication::audit(std::string_view id, std::string_view action, std::string_view outcome,
                              std::string_view principal_id, std::string_view resource_type,
                              std::string_view resource_id) const {
    write_structured_log(
        axk::server::structured_audit_log(id, action, outcome, principal_id, resource_type, resource_id));
}

std::optional<crow::response> ServerApplication::guard(const crow::request &request, std::string_view id) const {
    if (!origin_allowed(request)) {
        audit(id, "origin_policy", "denied");
        return error_response(403, {"origin_denied", "request origin is not allowed"}, id);
    }
    if (!authenticated_principal(request)) {
        audit(id, "authentication", "denied");
        return error_response(401, {"authentication_required", "valid bearer authentication is required"}, id);
    }
    return std::nullopt;
}

crow::response ServerApplication::preflight_response(const crow::request &request) const {
    return crow::response{origin_allowed(request) ? 204 : 403};
}

crow::response ServerApplication::capability_response(const crow::request &request) const {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    Json operations = Json::array();
    for (const auto &entry : registry_.entries()) {
        operations.push_back({
            {"id", entry.descriptor.id},
            {"method", axk::app::http_method_name(entry.descriptor.method)},
            {"route", entry.descriptor.route},
            {"mode", entry.descriptor.mode == axk::app::ExecutionMode::request ? "REQUEST" : "JOB"},
            {"operationClass", entry.descriptor.operation_class == axk::app::OperationClass::read ? "READ" : "WRITE"},
            {"requiresIdempotency", entry.descriptor.requires_idempotency},
            {"cliParity", entry.descriptor.cli_parity},
            {"variant", entry.descriptor.variant.empty() ? Json{} : Json(entry.descriptor.variant)},
            {"requestSchema", entry.descriptor.request_schema},
            {"resultSchema", entry.descriptor.result_schema},
            {"implemented", entry.implemented},
        });
    }
    const Json limits{{"maximumJsonBytes", config_.maximum_json_bytes},
                      {"maximumJsonDepth", config_.maximum_json_depth},
                      {"maximumJsonNodes", config_.maximum_json_nodes},
                      {"maximumJsonContainerItems", config_.maximum_json_container_items},
                      {"maximumJsonStringBytes", config_.maximum_json_string_bytes},
                      {"maximumAlterationJournalBytes", config_.maximum_alteration_journal_bytes},
                      {"maximumUploadBytes", config_.maximum_upload_bytes},
                      {"maximumUploadTotalBytes", config_.maximum_upload_total_bytes},
                      {"maximumUploads", config_.maximum_uploads},
                      {"maximumUploadChunkBytes", config_.maximum_upload_chunk_bytes},
                      {"maximumDownloadRangeBytes", config_.maximum_download_range_bytes},
                      {"maximumAuditionBundleBytes", config_.maximum_audition_bundle_bytes},
                      {"maximumDownloadArchiveBytes", config_.maximum_download_archive_bytes},
                      {"maximumDownloadArchiveTotalBytes", config_.maximum_download_archive_total_bytes},
                      {"maximumDownloadArchiveEntries", config_.maximum_download_archive_entries},
                      {"maximumDownloadArchiveDepth", config_.maximum_download_archive_depth},
                      {"maximumDownloadArchivePathBytes", config_.maximum_download_archive_path_bytes},
                      {"maximumConcurrentArchiveDownloads", config_.maximum_concurrent_archive_downloads},
                      {"downloadArchiveRetentionSeconds", config_.download_archive_retention_seconds},
                      {"maximumWebsocketDeliveryEvents", config_.maximum_websocket_delivery_events},
                      {"maximumWebsocketDeliveryBytes", config_.maximum_websocket_delivery_bytes},
                      {"maximumQueuedJobs", config_.maximum_queued_jobs},
                      {"maximumImageSessions", config_.maximum_image_sessions},
                      {"maximumMediaBuildObjectBytes", config_.maximum_media_build_object_bytes},
                      {"maximumMediaBuildPayloadBytes", config_.maximum_media_build_payload_bytes},
                      {"maximumMediaBuildOutputBytes", config_.maximum_media_build_output_bytes},
                      {"maximumPageSize", config_.maximum_page_size}};
    Json sample_rates = Json::array();
    for (const auto rate : axk::supported_sampler_sample_rates)
        sample_rates.push_back(rate);
    Json sample_widths = Json::array();
    for (const auto width : axk::supported_sampler_output_sample_widths_bits)
        sample_widths.push_back(width);
    const Json audio_import{{"supportedSampleRates", std::move(sample_rates)},
                            {"defaultUnsupportedSampleRate", axk::default_sampler_sample_rate},
                            {"supportedOutputSampleWidthsBits", std::move(sample_widths)},
                            {"sampleWidthPolicy", axk::sampler_sample_width_policy}};
    return json_response(200,
                         {{"data",
                           {{"apiVersion", "v1"},
                            {"operations", std::move(operations)},
                            {"limits", limits},
                            {"audioImport", audio_import}}},
                          {"meta", {{"requestId", id}}}},
                         id);
}

const Json &ServerApplication::openapi_document() const noexcept { return openapi_document_; }

axk::app::Result<Json> ServerApplication::parse_validated_json_body(const crow::request &request,
                                                                    std::string_view schema_name) const {
    auto parsed = parse_json_body(request, config_);
    if (!parsed)
        return std::unexpected(parsed.error());
    if (const auto valid = openapi_validator_.validate(schema_name, *parsed); !valid)
        return std::unexpected(valid.error());
    return parsed;
}

} // namespace axk::server::detail
