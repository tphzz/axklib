#include "axklib/server/config.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>

#include <nlohmann/json.hpp>

#include "axklib/application/secure_random.hpp"
#include "environment.hpp"

namespace {

axk::app::Error argument_error(std::string message) { return {"invalid_server_argument", std::move(message)}; }

template <typename Integer> axk::app::Result<Integer> parse_integer(std::string_view text, std::string_view name) {
    Integer value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        return std::unexpected(argument_error(std::string{name} + " must be an integer"));
    return value;
}

bool is_loopback(std::string_view address) {
    return address == "127.0.0.1" || address == "::1" || address == "localhost";
}

bool valid_principal_id(std::string_view value) {
    return !value.empty() && value.size() <= 64U && std::ranges::all_of(value, [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '-' || character == '_' || character == '.';
    });
}

bool valid_sha256(std::string_view value) {
    return value.size() == 64U && std::ranges::all_of(value, [](unsigned char character) {
               return std::isdigit(character) != 0 || (character >= 'a' && character <= 'f');
           });
}

axk::app::Result<void> apply_config_file(axk::server::Config &config, const std::filesystem::path &path) {
    using Json = nlohmann::json;
    std::ifstream input{path, std::ios::binary};
    if (!input)
        return std::unexpected(argument_error("could not open server config file"));
    Json document;
    try {
        document = Json::parse(input);
    } catch (const Json::exception &) {
        return std::unexpected(argument_error("server config file is not valid JSON"));
    }
    if (!document.is_object())
        return std::unexpected(argument_error("server config file must contain one JSON object"));

    static const std::set<std::string, std::less<>> known_keys{
        "allowedOrigins",
        "bearerToken",
        "bindAddress",
        "connectionFile",
        "eventTicketTtlSeconds",
        "imageIdleSeconds",
        "jobRetentionSeconds",
        "jobWorkerThreads",
        "maximumDownloadRangeBytes",
        "maximumDownloadArchiveBytes",
        "maximumDownloadArchiveEntries",
        "maximumDownloadArchiveTotalBytes",
        "maximumEventTickets",
        "maximumImageSessions",
        "maximumJsonBytes",
        "maximumJsonContainerItems",
        "maximumJsonDepth",
        "maximumJsonNodes",
        "maximumJsonStringBytes",
        "maximumPageSize",
        "maximumQueuedJobs",
        "maximumRetainedJobs",
        "maximumUploadBytes",
        "maximumUploadChunkBytes",
        "maximumUploads",
        "maximumUploadTotalBytes",
        "maximumWebsocketDeliveryBytes",
        "maximumWebsocketDeliveryEvents",
        "maximumWebsocketPayloadBytes",
        "port",
        "replayEventsPerJob",
        "stateDirectory",
        "streamThresholdBytes",
        "tokenHashes",
        "uploadRetentionSeconds",
        "downloadArchiveRetentionSeconds",
        "workerThreads",
        "writeJobWorkerThreads",
        "workspaceStore",
    };
    for (const auto &[key, value] : document.items()) {
        static_cast<void>(value);
        if (!known_keys.contains(key))
            return std::unexpected(argument_error("unknown server config key: " + key));
    }

    const auto assign = [&document]<typename Value>(std::string_view key, Value &target) {
        if (const auto found = document.find(key); found != document.end())
            target = found->template get<Value>();
    };
    try {
        assign("bindAddress", config.bind_address);
        assign("port", config.port);
        assign("bearerToken", config.bearer_token);
        assign("stateDirectory", config.state_directory);
        assign("connectionFile", config.connection_file);
        assign("workspaceStore", config.workspace_store);
        assign("maximumJsonBytes", config.maximum_json_bytes);
        assign("maximumJsonDepth", config.maximum_json_depth);
        assign("maximumJsonNodes", config.maximum_json_nodes);
        assign("maximumJsonContainerItems", config.maximum_json_container_items);
        assign("maximumJsonStringBytes", config.maximum_json_string_bytes);
        assign("streamThresholdBytes", config.stream_threshold_bytes);
        assign("maximumWebsocketPayloadBytes", config.maximum_websocket_payload_bytes);
        assign("maximumWebsocketDeliveryEvents", config.maximum_websocket_delivery_events);
        assign("maximumWebsocketDeliveryBytes", config.maximum_websocket_delivery_bytes);
        assign("maximumQueuedJobs", config.maximum_queued_jobs);
        assign("maximumRetainedJobs", config.maximum_retained_jobs);
        assign("replayEventsPerJob", config.replay_events_per_job);
        assign("maximumEventTickets", config.maximum_event_tickets);
        assign("eventTicketTtlSeconds", config.event_ticket_ttl_seconds);
        assign("jobRetentionSeconds", config.job_retention_seconds);
        assign("maximumUploadBytes", config.maximum_upload_bytes);
        assign("maximumUploadTotalBytes", config.maximum_upload_total_bytes);
        assign("maximumUploads", config.maximum_uploads);
        assign("maximumUploadChunkBytes", config.maximum_upload_chunk_bytes);
        assign("maximumDownloadRangeBytes", config.maximum_download_range_bytes);
        assign("maximumDownloadArchiveBytes", config.maximum_download_archive_bytes);
        assign("maximumDownloadArchiveTotalBytes", config.maximum_download_archive_total_bytes);
        assign("maximumDownloadArchiveEntries", config.maximum_download_archive_entries);
        assign("downloadArchiveRetentionSeconds", config.download_archive_retention_seconds);
        assign("uploadRetentionSeconds", config.upload_retention_seconds);
        assign("maximumImageSessions", config.maximum_image_sessions);
        assign("maximumPageSize", config.maximum_page_size);
        assign("imageIdleSeconds", config.image_idle_seconds);
        assign("workerThreads", config.worker_threads);
        assign("jobWorkerThreads", config.job_worker_threads);
        assign("writeJobWorkerThreads", config.write_job_worker_threads);

        if (const auto found = document.find("allowedOrigins"); found != document.end())
            config.allowed_origins = found->get<std::vector<std::string>>();
        if (const auto found = document.find("tokenHashes"); found != document.end()) {
            config.token_hashes.clear();
            for (const auto &value : *found) {
                config.token_hashes.push_back(
                    {value.at("principalId").get<std::string>(), value.at("sha256").get<std::string>()});
            }
        }
    } catch (const Json::exception &) {
        return std::unexpected(argument_error("server config value does not match its documented type"));
    }
    return {};
}

template <typename Value> axk::app::Result<void> apply_integer_environment(std::string_view name, Value &target) {
    const auto raw = axk::server::detail::environment_variable(name);
    if (!raw)
        return {};
    auto parsed = parse_integer<Value>(*raw, name);
    if (!parsed)
        return std::unexpected(parsed.error());
    target = *parsed;
    return {};
}

axk::app::Result<void> apply_environment(axk::server::Config &config) {
    const auto assign_text = [](const char *name, auto &target) {
        if (const auto value = axk::server::detail::environment_variable(name))
            target = *value;
    };
    assign_text("AXKLIB_SERVER_BIND", config.bind_address);
    assign_text("AXKLIB_SERVER_TOKEN", config.bearer_token);
    assign_text("AXKLIB_SERVER_STATE_DIRECTORY", config.state_directory);
    assign_text("AXKLIB_SERVER_CONNECTION_FILE", config.connection_file);
    for (auto result : {apply_integer_environment("AXKLIB_SERVER_PORT", config.port),
                        apply_integer_environment("AXKLIB_SERVER_WORKERS", config.worker_threads),
                        apply_integer_environment("AXKLIB_SERVER_JOB_WORKERS", config.job_worker_threads),
                        apply_integer_environment("AXKLIB_SERVER_WRITE_JOB_WORKERS", config.write_job_worker_threads),
                        apply_integer_environment("AXKLIB_SERVER_MAX_QUEUED_JOBS", config.maximum_queued_jobs)}) {
        if (!result)
            return std::unexpected(result.error());
    }
    return {};
}

} // namespace

axk::app::Result<axk::server::CommandLine> axk::server::parse_command_line(int argc, char **argv) {
    CommandLine command_line;
    std::optional<std::filesystem::path> config_path;
    bool informational{};
    bool sidecar_mode_requested{};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index] == nullptr ? "" : argv[index]};
        if (argument == "--help" || argument == "-h" || argument == "--print-version")
            informational = true;
        if (argument == "--connection-file")
            sidecar_mode_requested = true;
        if (argument != "--config")
            continue;
        if (config_path)
            return std::unexpected(argument_error("--config may be specified only once"));
        if (index + 1 >= argc || argv[index + 1] == nullptr)
            return std::unexpected(argument_error("--config requires a value"));
        config_path = std::filesystem::path{argv[++index]};
    }
    if (!informational) {
        if (!sidecar_mode_requested && !config_path) {
            if (const auto configured = detail::environment_variable("AXKLIB_SERVER_CONFIG"))
                config_path = std::filesystem::path{*configured};
        }
        if (config_path) {
            if (auto loaded = apply_config_file(command_line.config, *config_path); !loaded)
                return std::unexpected(loaded.error());
        }
        if (!sidecar_mode_requested) {
            if (auto environment = apply_environment(command_line.config); !environment)
                return std::unexpected(environment.error());
        }
    }
    bool replaced_token_hashes{};
    bool replaced_origins{};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index] == nullptr ? "" : argv[index]};
        const auto value_after = [&](std::string_view option) -> app::Result<std::string_view> {
            if (index + 1 >= argc || argv[index + 1] == nullptr)
                return std::unexpected(argument_error(std::string{option} + " requires a value"));
            ++index;
            return std::string_view{argv[index]};
        };
        if (argument == "--help" || argument == "-h") {
            command_line.print_help = true;
        } else if (argument == "--print-version") {
            command_line.print_version = true;
        } else if (argument == "--config") {
            auto value = value_after(argument);
            if (!value)
                return std::unexpected(value.error());
        } else if (argument == "--bind") {
            auto value = value_after(argument);
            if (!value)
                return std::unexpected(value.error());
            command_line.config.bind_address = *value;
        } else if (argument == "--port") {
            auto value = value_after(argument);
            if (!value)
                return std::unexpected(value.error());
            auto port = parse_integer<std::uint32_t>(*value, "--port");
            if (!port || *port > std::numeric_limits<std::uint16_t>::max())
                return std::unexpected(argument_error("--port must be between 0 and 65535"));
            command_line.config.port = static_cast<std::uint16_t>(*port);
        } else if (argument == "--token") {
            auto value = value_after(argument);
            if (!value)
                return std::unexpected(value.error());
            command_line.config.bearer_token = *value;
        } else if (argument == "--token-sha256") {
            auto value = value_after(argument);
            if (!value)
                return std::unexpected(value.error());
            const auto separator = value->find('=');
            if (separator == std::string_view::npos || separator == 0U || separator + 1U == value->size())
                return std::unexpected(argument_error("--token-sha256 must use PRINCIPAL=LOWERCASE_SHA256"));
            if (!replaced_token_hashes) {
                command_line.config.token_hashes.clear();
                replaced_token_hashes = true;
            }
            command_line.config.token_hashes.push_back(
                {std::string{value->substr(0U, separator)}, std::string{value->substr(separator + 1U)}});
        } else if (argument == "--allow-origin") {
            auto value = value_after(argument);
            if (!value)
                return std::unexpected(value.error());
            if (!replaced_origins) {
                command_line.config.allowed_origins.clear();
                replaced_origins = true;
            }
            command_line.config.allowed_origins.emplace_back(*value);
        } else if (argument == "--workspace-store") {
            auto value = value_after(argument);
            if (!value)
                return std::unexpected(value.error());
            command_line.config.workspace_store = std::filesystem::path{*value};
        } else if (argument == "--state-directory") {
            auto value = value_after(argument);
            if (!value)
                return std::unexpected(value.error());
            command_line.config.state_directory = std::filesystem::path{*value};
        } else if (argument == "--connection-file") {
            auto value = value_after(argument);
            if (!value)
                return std::unexpected(value.error());
            command_line.config.connection_file = std::filesystem::path{*value};
        } else if (argument == "--parent-pid") {
            auto value = value_after(argument);
            if (!value)
                return std::unexpected(value.error());
            auto process_id = parse_integer<std::uint64_t>(*value, "--parent-pid");
            if (!process_id || *process_id == 0U)
                return std::unexpected(argument_error("--parent-pid must be a positive integer"));
            command_line.config.parent_process_id = *process_id;
        } else if (argument == "--workers") {
            auto value = value_after(argument);
            if (!value)
                return std::unexpected(value.error());
            auto workers = parse_integer<std::uint32_t>(*value, "--workers");
            if (!workers || *workers < 2U || *workers > 64U)
                return std::unexpected(argument_error("--workers must be between 2 and 64"));
            command_line.config.worker_threads = static_cast<std::uint16_t>(*workers);
        } else if (argument == "--job-workers") {
            auto value = value_after(argument);
            if (!value)
                return std::unexpected(value.error());
            auto workers = parse_integer<std::uint32_t>(*value, "--job-workers");
            if (!workers || *workers < 1U || *workers > 32U)
                return std::unexpected(argument_error("--job-workers must be between 1 and 32"));
            command_line.config.job_worker_threads = static_cast<std::uint16_t>(*workers);
        } else if (argument == "--max-queued-jobs") {
            auto value = value_after(argument);
            if (!value)
                return std::unexpected(value.error());
            auto jobs = parse_integer<std::size_t>(*value, "--max-queued-jobs");
            if (!jobs || *jobs < 1U || *jobs > 10000U)
                return std::unexpected(argument_error("--max-queued-jobs must be between 1 and 10000"));
            command_line.config.maximum_queued_jobs = *jobs;
        } else if (argument == "--write-job-workers") {
            auto value = value_after(argument);
            if (!value)
                return std::unexpected(value.error());
            auto workers = parse_integer<std::uint32_t>(*value, "--write-job-workers");
            if (!workers || *workers < 1U || *workers > 16U)
                return std::unexpected(argument_error("--write-job-workers must be between 1 and 16"));
            command_line.config.write_job_worker_threads = static_cast<std::uint16_t>(*workers);
        } else if (argument == "--max-retained-jobs") {
            auto value = value_after(argument);
            if (!value)
                return std::unexpected(value.error());
            auto jobs = parse_integer<std::size_t>(*value, "--max-retained-jobs");
            if (!jobs || *jobs < 1U || *jobs > 100000U)
                return std::unexpected(argument_error("--max-retained-jobs must be between 1 and 100000"));
            command_line.config.maximum_retained_jobs = *jobs;
        } else if (argument == "--job-retention-seconds") {
            auto value = value_after(argument);
            if (!value)
                return std::unexpected(value.error());
            auto seconds = parse_integer<std::uint32_t>(*value, "--job-retention-seconds");
            if (!seconds || *seconds < 1U || *seconds > 86400U)
                return std::unexpected(argument_error("--job-retention-seconds must be between 1 and 86400"));
            command_line.config.job_retention_seconds = *seconds;
        } else if (argument == "--job-replay-events") {
            auto value = value_after(argument);
            if (!value)
                return std::unexpected(value.error());
            auto events = parse_integer<std::size_t>(*value, "--job-replay-events");
            if (!events || *events < 1U || *events > 4096U)
                return std::unexpected(argument_error("--job-replay-events must be between 1 and 4096"));
            command_line.config.replay_events_per_job = *events;
        } else if (argument == "--max-event-tickets") {
            auto value = value_after(argument);
            if (!value)
                return std::unexpected(value.error());
            auto tickets = parse_integer<std::size_t>(*value, "--max-event-tickets");
            if (!tickets || *tickets < 1U || *tickets > 100000U)
                return std::unexpected(argument_error("--max-event-tickets must be between 1 and 100000"));
            command_line.config.maximum_event_tickets = *tickets;
        } else if (argument == "--max-image-sessions") {
            auto value = value_after(argument);
            if (!value)
                return std::unexpected(value.error());
            auto sessions = parse_integer<std::size_t>(*value, "--max-image-sessions");
            if (!sessions || *sessions < 1U || *sessions > 1024U)
                return std::unexpected(argument_error("--max-image-sessions must be between 1 and 1024"));
            command_line.config.maximum_image_sessions = *sessions;
        } else if (argument == "--max-page-size") {
            auto value = value_after(argument);
            if (!value)
                return std::unexpected(value.error());
            auto page_size = parse_integer<std::size_t>(*value, "--max-page-size");
            if (!page_size || *page_size < 1U || *page_size > 5000U)
                return std::unexpected(argument_error("--max-page-size must be between 1 and 5000"));
            command_line.config.maximum_page_size = *page_size;
        } else if (argument == "--image-idle-seconds") {
            auto value = value_after(argument);
            if (!value)
                return std::unexpected(value.error());
            auto seconds = parse_integer<std::uint32_t>(*value, "--image-idle-seconds");
            if (!seconds || *seconds < 1U || *seconds > 86400U)
                return std::unexpected(argument_error("--image-idle-seconds must be between 1 and 86400"));
            command_line.config.image_idle_seconds = *seconds;
        } else {
            return std::unexpected(argument_error("unknown option: " + std::string{argument}));
        }
    }
    if (!command_line.config.connection_file.empty() && command_line.config.bearer_token.empty()) {
        auto token = app::secure_random_hex(32U);
        if (!token)
            return std::unexpected(argument_error(token.error().message));
        command_line.config.bearer_token = std::move(*token);
    }
    if (!command_line.print_help && !command_line.print_version) {
        if (auto valid = validate_config(command_line.config); !valid)
            return std::unexpected(valid.error());
    }
    return command_line;
}

std::string axk::server::command_line_help() {
    return "axklib-server\n\n"
           "Usage: axklib-server [OPTIONS]\n\n"
           "Options:\n"
           "  --config PATH          strict JSON server configuration file\n"
           "  --bind ADDRESS          listen address (default: 127.0.0.1)\n"
           "  --port PORT             listen port, or 0 for an ephemeral port\n"
           "  --token TOKEN           bearer token (generated in sidecar mode)\n"
           "  --token-sha256 ID=HASH named LAN bearer-token SHA-256; repeatable\n"
           "  --allow-origin ORIGIN   exact permitted browser origin; repeatable\n"
           "  --workspace-store PATH persisted per-user workspace registry\n"
           "  --state-directory PATH private upload and sidecar state directory\n"
           "  --connection-file PATH atomically publish sidecar connection metadata\n"
           "  --parent-pid PID        stop when the owning desktop process exits\n"
           "  --workers COUNT         Crow worker threads (2-64)\n"
           "  --job-workers COUNT     application job workers (1-32)\n"
           "  --write-job-workers N   image-writing job workers (1-16)\n"
           "  --max-queued-jobs COUNT bounded pending job capacity (1-10000)\n"
           "  --max-retained-jobs N   retained job capacity (1-100000)\n"
           "  --job-retention-seconds N terminal job retention (1-86400)\n"
           "  --job-replay-events N   retained events per job (1-4096)\n"
           "  --max-event-tickets N   outstanding WebSocket ticket capacity (1-100000)\n"
           "  --max-image-sessions N  open image session capacity (1-1024)\n"
           "  --max-page-size N       maximum items in one API page (1-5000)\n"
           "  --image-idle-seconds N  idle image session retention (1-86400)\n"
           "  --print-version         print semantic and source version, then exit\n"
           "  -h, --help              show this help\n";
}

axk::app::Result<void> axk::server::validate_config(const Config &config) {
    if (config.bind_address.empty())
        return std::unexpected(argument_error("listen address must not be empty"));
    if (config.parent_process_id != 0U && config.connection_file.empty())
        return std::unexpected(argument_error("--parent-pid requires sidecar connection-file mode"));
    const auto loopback = is_loopback(config.bind_address);
    if (config.bearer_token.empty() && config.token_hashes.empty())
        return std::unexpected(argument_error("at least one bearer credential is required"));
    if (!config.bearer_token.empty() && config.bearer_token.size() < 16U)
        return std::unexpected(argument_error("plaintext loopback bearer token must contain at least 16 bytes"));
    std::vector<std::string> principals;
    for (const auto &token : config.token_hashes) {
        if (!valid_principal_id(token.principal_id) || !valid_sha256(token.sha256))
            return std::unexpected(argument_error("LAN token hashes require a valid principal and lowercase SHA-256"));
        if (std::ranges::find(principals, token.principal_id) != principals.end())
            return std::unexpected(argument_error("LAN token principal IDs must be unique"));
        principals.push_back(token.principal_id);
    }
    if (!loopback) {
        if (!config.bearer_token.empty())
            return std::unexpected(
                argument_error("non-loopback listening rejects plaintext bearer-token configuration"));
        if (config.token_hashes.empty())
            return std::unexpected(argument_error("non-loopback listening requires at least one hashed bearer token"));
        if (config.allowed_origins.empty())
            return std::unexpected(argument_error("non-loopback listening requires at least one exact allowed origin"));
    }
    if (std::ranges::any_of(config.allowed_origins,
                            [](const auto &origin) { return origin.empty() || origin == "*"; })) {
        return std::unexpected(argument_error("allowed origins must be exact non-wildcard values"));
    }
    if (!config.connection_file.empty() && !loopback)
        return std::unexpected(argument_error("sidecar connection files require a loopback listen address"));
    if (!config.connection_file.empty() && !config.token_hashes.empty())
        return std::unexpected(argument_error("sidecar mode does not accept LAN token hashes"));
    if (config.maximum_json_bytes == 0U || config.maximum_json_depth == 0U || config.maximum_json_depth > 256U ||
        config.maximum_json_nodes == 0U || config.maximum_json_container_items == 0U ||
        config.maximum_json_string_bytes == 0U || config.stream_threshold_bytes == 0U) {
        return std::unexpected(argument_error("server JSON and stream limits must be nonzero"));
    }
    if (config.worker_threads < 2U)
        return std::unexpected(argument_error("Crow requires at least two worker threads"));
    if (config.job_worker_threads == 0U || config.job_worker_threads > 32U)
        return std::unexpected(argument_error("job worker count must be between 1 and 32"));
    if (config.write_job_worker_threads == 0U || config.write_job_worker_threads > 16U)
        return std::unexpected(argument_error("write job worker count must be between 1 and 16"));
    if (config.maximum_queued_jobs == 0U || config.maximum_queued_jobs > 10000U)
        return std::unexpected(argument_error("maximum queued jobs must be between 1 and 10000"));
    if (config.replay_events_per_job == 0U || config.replay_events_per_job > 4096U)
        return std::unexpected(argument_error("job replay events must be between 1 and 4096"));
    if (config.maximum_retained_jobs == 0U || config.maximum_retained_jobs > 100000U)
        return std::unexpected(argument_error("maximum retained jobs must be between 1 and 100000"));
    if (config.job_retention_seconds == 0U || config.job_retention_seconds > 86400U)
        return std::unexpected(argument_error("job retention must be between 1 and 86400 seconds"));
    if (config.maximum_websocket_payload_bytes == 0U || config.maximum_websocket_payload_bytes > 1024U * 1024U)
        return std::unexpected(argument_error("WebSocket payload limit must be between 1 byte and 1 MiB"));
    if (config.maximum_websocket_delivery_events == 0U || config.maximum_websocket_delivery_events > 100000U ||
        config.maximum_websocket_delivery_bytes == 0U ||
        config.maximum_websocket_delivery_bytes > 1024ULL * 1024ULL * 1024ULL) {
        return std::unexpected(argument_error("WebSocket delivery budgets are invalid"));
    }
    if (config.maximum_event_tickets == 0U || config.maximum_event_tickets > 100000U ||
        config.event_ticket_ttl_seconds == 0U || config.event_ticket_ttl_seconds > 300U) {
        return std::unexpected(argument_error("event ticket limits are invalid"));
    }
    if (config.maximum_upload_bytes == 0U || config.maximum_upload_total_bytes < config.maximum_upload_bytes ||
        config.maximum_uploads == 0U || config.maximum_upload_chunk_bytes == 0U ||
        config.maximum_upload_chunk_bytes > config.maximum_upload_bytes || config.maximum_download_range_bytes == 0U ||
        config.upload_retention_seconds == 0U || config.upload_retention_seconds > 86400U) {
        return std::unexpected(argument_error("upload and download limits are invalid"));
    }
    if (config.maximum_download_archive_bytes == 0U ||
        config.maximum_download_archive_total_bytes < config.maximum_download_archive_bytes ||
        config.maximum_download_archive_entries == 0U || config.maximum_download_archive_entries > 1000000U ||
        config.download_archive_retention_seconds == 0U || config.download_archive_retention_seconds > 86400U) {
        return std::unexpected(argument_error("download archive limits are invalid"));
    }
    if (config.maximum_image_sessions == 0U || config.maximum_image_sessions > 1024U ||
        config.maximum_page_size == 0U || config.maximum_page_size > 5000U || config.image_idle_seconds == 0U ||
        config.image_idle_seconds > 86400U) {
        return std::unexpected(argument_error("image session limits are invalid"));
    }
    if (!config.workspace_store.empty() && !config.workspace_store.is_absolute())
        return std::unexpected(argument_error("workspace store path must be absolute"));
    return {};
}
