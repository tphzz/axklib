#include "server_support.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <random>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>

#include "axklib/server/request_validation.hpp"
#include "axklib/server/telemetry.hpp"
#include "environment.hpp"
#include "transport.hpp"

namespace axk::server::detail {

std::function<void(const Json &)> operation_diagnostic_sink() {
    static const bool enabled = [] {
        const auto level = axk::server::detail::environment_variable("AXKLIB_SERVER_LOG_LEVEL");
        if (!level)
            return false;
        auto normalized = *level;
        std::ranges::transform(normalized, normalized.begin(),
                               [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        return normalized == "debug" || normalized == "trace";
    }();
    if (!enabled)
        return {};
    return [](const Json &event) { write_structured_log(event.dump()); };
}

std::string readiness_check_name() {
    std::random_device source;
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string result{".axklib-readiness-"};
    result.reserve(result.size() + 32U);
    for (std::size_t index = 0; index < 16U; ++index) {
        const auto value = static_cast<unsigned int>(source());
        result.push_back(digits[(value >> 4U) & 0x0fU]);
        result.push_back(digits[value & 0x0fU]);
    }
    return result;
}

bool writable_directory(const std::filesystem::path &directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error || !std::filesystem::is_directory(directory, error) || error)
        return false;
    const auto check_path = directory / readiness_check_name();
    {
        std::ofstream output{check_path, std::ios::binary | std::ios::trunc};
        if (!output)
            return false;
        output << "ready\n";
        output.flush();
        if (!output) {
            std::filesystem::remove(check_path, error);
            return false;
        }
    }
    const auto removed = std::filesystem::remove(check_path, error);
    return removed && !error;
}

bool cleanup_complete(const std::filesystem::path &directory) {
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator{directory, error}) {
        if (error)
            return false;
        if (entry.path().extension() == ".part" || entry.path().extension() == ".upload")
            return false;
    }
    return !error;
}

axk::app::Result<Json> parse_json_body(const crow::request &request, const axk::server::Config &config) {
    return axk::server::parse_json_request(request.body, config);
}

crow::response json_response(int status, const Json &body, std::string_view request_id_value) {
    crow::response response{status, "application/json", body.dump()};
    response.set_header("X-Request-Id", std::string{request_id_value});
    response.set_header("Cache-Control", "no-store");
    return response;
}

crow::response error_response(int status, const axk::app::Error &error, std::string_view request_id_value) {
    Json context = Json::object();
    if (error.context.partition_index)
        context["partitionIndex"] = *error.context.partition_index;
    if (error.context.volume_name)
        context["volumeName"] = *error.context.volume_name;
    if (error.context.object_type)
        context["objectType"] = *error.context.object_type;
    if (error.context.object_name)
        context["objectName"] = *error.context.object_name;
    if (error.context.object_id)
        context["objectId"] = *error.context.object_id;
    if (error.context.relative_path)
        context["relativePath"] = *error.context.relative_path;
    auto response = json_response(status,
                                  {{"error",
                                    {{"code", error.code},
                                     {"message", error.message},
                                     {"context", std::move(context)},
                                     {"requestId", request_id_value},
                                     {"retryable", error.retryable}}}},
                                  request_id_value);
    if (status == 429 && error.retryable)
        response.set_header("Retry-After", "1");
    return response;
}

int status_for_error(const axk::app::Error &error, int fallback) {
    if (error.code == "upload_storage_unavailable" || error.code == "archive_storage_unavailable")
        return 503;
    if (error.code == "request_too_large" || error.code == "json_structure_too_large")
        return 413;
    if (error.code == "job_not_found")
        return 404;
    if (error.code == "upload_not_found")
        return 404;
    if (error.code == "image_not_found" || error.code == "audition_not_found")
        return 404;
    if (error.code == "package_plan_not_found")
        return 404;
    if (error.code == "job_event_replay_expired")
        return 409;
    if (error.code == "job_queue_full" || error.code == "event_ticket_capacity_exhausted" ||
        error.code == "image_capacity_exhausted")
        return 429;
    if (error.code == "job_capacity_full")
        return 429;
    if (error.code == "upload_quota_exceeded")
        return 429;
    if (error.code == "download_archive_quota_exceeded")
        return 429;
    if (error.code == "archive_download_capacity_exhausted")
        return 429;
    if (error.code == "package_plan_capacity")
        return 429;
    if (error.code == "download_archive_too_large")
        return 413;
    if (error.code == "image_build_too_large")
        return 413;
    if (error.code == "upload_type_not_allowed")
        return 415;
    if (error.code == "invalid_upload_chunk" || error.code == "upload_not_ready")
        return 409;
    if (error.code == "upload_in_use" || error.code == "upload_materialization_failed")
        return 409;
    if (error.code == "download_archive_not_found")
        return 404;
    if (error.code == "archive_in_use")
        return 409;
    if (error.code == "idempotency_conflict")
        return 409;
    if (error.code == "package_plan_in_use" || error.code == "package_plan_conflicts" ||
        error.code == "package_plan_stale" || error.code == "image_revision_stale")
        return 409;
    if (error.code == "destination_reserved")
        return 409;
    if (error.code == "output_exists")
        return 409;
    if (error.code == "entry_not_found")
        return 404;
    if (error.code == "read_only_root")
        return 403;
    if (error.code == "directory_not_empty" || error.code == "entry_in_use")
        return 409;
    if (error.code == "entry_mutation_failed")
        return 500;
    if (error.code == "operation_not_implemented")
        return 501;
    if (error.code == "invalid_request" || error.code == "invalid_page" || error.code == "invalid_cursor" ||
        error.code == "unknown_operation" || error.code == "invalid_execution_mode" ||
        error.code == "idempotency_key_required" || error.code == "invalid_idempotency_key") {
        return 400;
    }
    return fallback;
}

bool requests_subprotocol(const crow::request &request, std::string_view expected) {
    std::string_view protocols{request.get_header_value("Sec-WebSocket-Protocol")};
    while (!protocols.empty()) {
        const auto separator = protocols.find(',');
        auto candidate = protocols.substr(0U, separator);
        while (!candidate.empty() && std::isspace(static_cast<unsigned char>(candidate.front())) != 0)
            candidate.remove_prefix(1U);
        while (!candidate.empty() && std::isspace(static_cast<unsigned char>(candidate.back())) != 0)
            candidate.remove_suffix(1U);
        if (candidate == expected)
            return true;
        if (separator == std::string_view::npos)
            break;
        protocols.remove_prefix(separator + 1U);
    }
    return false;
}

std::optional<std::uint64_t> parse_sequence(const char *text) {
    if (text == nullptr || *text == '\0')
        return 0U;
    const std::string_view value{text};
    std::uint64_t sequence{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), sequence);
    if (error != std::errc{} || end != value.data() + value.size())
        return std::nullopt;
    return sequence;
}

std::optional<std::uint64_t> parse_unsigned(std::string_view value) {
    if (value.empty())
        return std::nullopt;
    std::uint64_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size())
        return std::nullopt;
    return result;
}

std::optional<axk::app::UploadKind> parse_upload_kind(std::string_view value) {
    if (value == "AUDIO")
        return axk::app::UploadKind::audio;
    if (value == "MIDI")
        return axk::app::UploadKind::midi;
    if (value == "PACKAGE")
        return axk::app::UploadKind::package;
    if (value == "MANIFEST")
        return axk::app::UploadKind::manifest;
    if (value == "DISK_IMAGE")
        return axk::app::UploadKind::disk_image;
    return std::nullopt;
}

Json upload_json(const axk::app::UploadSnapshot &upload) {
    return {{"uploadId", upload.reference.upload_id},
            {"filename", upload.filename},
            {"kind", axk::app::upload_kind_name(upload.kind)},
            {"mediaType", upload.media_type},
            {"declaredSize", upload.declared_size},
            {"receivedSize", upload.received_size},
            {"state", axk::app::upload_state_name(upload.state)},
            {"expiresInSeconds", upload.expires_in_seconds}};
}

std::optional<ByteRange> parse_byte_range(std::string_view value, std::uint64_t file_size, std::size_t maximum_length) {
    constexpr std::string_view prefix{"bytes="};
    if (!value.starts_with(prefix) || value.find(',') != std::string_view::npos)
        return std::nullopt;
    value.remove_prefix(prefix.size());
    const auto separator = value.find('-');
    if (separator == std::string_view::npos || separator == 0U)
        return std::nullopt;
    const auto start = parse_unsigned(value.substr(0U, separator));
    if (!start || *start >= file_size)
        return std::nullopt;
    std::uint64_t end = file_size - 1U;
    if (separator + 1U < value.size()) {
        const auto parsed_end = parse_unsigned(value.substr(separator + 1U));
        if (!parsed_end || *parsed_end < *start)
            return std::nullopt;
        end = std::min(*parsed_end, end);
    }
    const auto length = end - *start + 1U;
    if (length > maximum_length)
        return std::nullopt;
    return ByteRange{*start, length};
}

} // namespace axk::server::detail
