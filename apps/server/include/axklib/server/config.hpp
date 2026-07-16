#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "axklib/application/contracts.hpp"

namespace axk::server {

struct TokenHash {
    std::string principal_id;
    std::string sha256;
};

struct Config {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{7331};
    std::string bearer_token;
    std::vector<TokenHash> token_hashes;
    std::vector<std::string> allowed_origins;
    std::filesystem::path workspace_store;
    std::filesystem::path state_directory;
    std::filesystem::path connection_file;
    std::uint64_t parent_process_id{};
    std::size_t maximum_json_bytes{1024U * 1024U};
    std::size_t maximum_json_depth{32U};
    std::size_t maximum_json_nodes{100000U};
    std::size_t maximum_json_container_items{10000U};
    std::size_t maximum_json_string_bytes{256U * 1024U};
    std::size_t stream_threshold_bytes{64U * 1024U};
    std::size_t maximum_websocket_payload_bytes{4096U};
    std::size_t maximum_websocket_delivery_events{1024U};
    std::uint64_t maximum_websocket_delivery_bytes{4ULL * 1024ULL * 1024ULL};
    std::size_t maximum_queued_jobs{64U};
    std::size_t maximum_retained_jobs{2048U};
    std::size_t replay_events_per_job{64U};
    std::size_t maximum_event_tickets{1024U};
    std::uint32_t event_ticket_ttl_seconds{30U};
    std::uint32_t job_retention_seconds{900U};
    std::uint64_t maximum_upload_bytes{4ULL * 1024ULL * 1024ULL * 1024ULL};
    std::uint64_t maximum_upload_total_bytes{8ULL * 1024ULL * 1024ULL * 1024ULL};
    std::size_t maximum_uploads{64U};
    std::size_t maximum_upload_chunk_bytes{1024U * 1024U};
    std::size_t maximum_download_range_bytes{8U * 1024U * 1024U};
    std::uint64_t maximum_download_archive_bytes{4ULL * 1024ULL * 1024ULL * 1024ULL};
    std::uint64_t maximum_download_archive_total_bytes{8ULL * 1024ULL * 1024ULL * 1024ULL};
    std::size_t maximum_download_archive_entries{100000U};
    std::uint32_t download_archive_retention_seconds{300U};
    std::uint32_t upload_retention_seconds{3600U};
    std::size_t maximum_image_sessions{32U};
    std::size_t maximum_page_size{500U};
    std::uint32_t image_idle_seconds{900U};
    std::uint16_t worker_threads{2};
    std::uint16_t job_worker_threads{2};
    std::uint16_t write_job_worker_threads{1};
};

struct CommandLine {
    Config config;
    bool print_help{};
    bool print_version{};
};

[[nodiscard]] app::Result<CommandLine> parse_command_line(int argc, char **argv);
[[nodiscard]] std::string command_line_help();
[[nodiscard]] app::Result<void> validate_config(const Config &config);

} // namespace axk::server
