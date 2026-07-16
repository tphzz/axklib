#include "axklib/server/server.hpp"

#include "axklib/server/contract.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <syncstream>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <crow.h>
#include <nlohmann/json.hpp>

#include "axklib/application/application_operations.hpp"
#include "axklib/application/download_archives.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/jobs.hpp"
#include "axklib/application/uploads.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/server/event_delivery_budget.hpp"
#include "axklib/server/event_dispatcher.hpp"
#include "axklib/server/event_tickets.hpp"
#include "axklib/server/job_json.hpp"
#include "axklib/server/process_lifetime.hpp"
#include "axklib/server/request_validation.hpp"
#include "axklib/server/telemetry.hpp"
#include "axklib/server/workspaces.hpp"
#include "axklib/utf8.hpp"
#include "axklib/version.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <aclapi.h>
#include <process.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

using Json = nlohmann::json;

constexpr std::size_t maximum_cursor_length = 512U;

constexpr std::string_view event_subprotocol{"axklib.events.v1"};

std::uint64_t process_id() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

axk::app::Result<void> write_owner_only_file(const std::filesystem::path &path, std::string_view content) {
#ifdef _WIN32
    HANDLE token{};
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0)
        return std::unexpected(axk::app::Error{"connection_file_failed", "could not inspect the process owner"});
    DWORD token_size{};
    static_cast<void>(GetTokenInformation(token, TokenUser, nullptr, 0U, &token_size));
    std::vector<std::byte> token_storage(token_size);
    if (token_size == 0U || GetTokenInformation(token, TokenUser, token_storage.data(), token_size, &token_size) == 0) {
        CloseHandle(token);
        return std::unexpected(axk::app::Error{"connection_file_failed", "could not inspect the process owner"});
    }
    const auto *token_user = reinterpret_cast<const TOKEN_USER *>(token_storage.data());
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = static_cast<LPWSTR>(token_user->User.Sid);
    PACL acl{};
    if (SetEntriesInAclW(1U, &access, nullptr, &acl) != ERROR_SUCCESS) {
        CloseHandle(token);
        return std::unexpected(
            axk::app::Error{"connection_file_failed", "could not create owner-only connection-file permissions"});
    }
    SECURITY_DESCRIPTOR descriptor{};
    const auto descriptor_ready = InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) != 0 &&
                                  SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) != 0 &&
                                  SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED) != 0;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
    HANDLE file = INVALID_HANDLE_VALUE;
    if (descriptor_ready) {
        file = CreateFileW(path.c_str(), GENERIC_WRITE, 0U, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    LocalFree(acl);
    CloseHandle(token);
    if (file == INVALID_HANDLE_VALUE)
        return std::unexpected(
            axk::app::Error{"connection_file_failed", "could not create the owner-only connection file"});
    std::size_t offset{};
    while (offset < content.size()) {
        const auto remaining = std::min<std::size_t>(content.size() - offset, std::numeric_limits<DWORD>::max());
        DWORD written{};
        if (WriteFile(file, content.data() + offset, static_cast<DWORD>(remaining), &written, nullptr) == 0 ||
            written == 0U) {
            CloseHandle(file);
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            return std::unexpected(axk::app::Error{"connection_file_failed", "could not write the connection file"});
        }
        offset += written;
    }
    if (FlushFileBuffers(file) == 0) {
        CloseHandle(file);
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return std::unexpected(axk::app::Error{"connection_file_failed", "could not flush the connection file"});
    }
    CloseHandle(file);
    return {};
#else
    const auto descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0)
        return std::unexpected(
            axk::app::Error{"connection_file_failed", "could not create the owner-only connection file"});
    std::size_t offset{};
    while (offset < content.size()) {
        const auto written = ::write(descriptor, content.data() + offset, content.size() - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            ::close(descriptor);
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            return std::unexpected(axk::app::Error{"connection_file_failed", "could not write the connection file"});
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(descriptor) != 0) {
        ::close(descriptor);
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return std::unexpected(axk::app::Error{"connection_file_failed", "could not flush the connection file"});
    }
    ::close(descriptor);
    return {};
#endif
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
        if (entry.path().extension() == ".part")
            return false;
    }
    return !error;
}

axk::app::Result<Json> parse_json_body(const crow::request &request, const axk::server::Config &config) {
    return axk::server::parse_json_request(request.body, config);
}

class ScopedConnectionFile {
  public:
    explicit ScopedConnectionFile(std::filesystem::path path) : path_(std::move(path)) {}

    ScopedConnectionFile(const ScopedConnectionFile &) = delete;
    ScopedConnectionFile &operator=(const ScopedConnectionFile &) = delete;

    ~ScopedConnectionFile() {
        if (published_) {
            std::error_code error;
            std::filesystem::remove(path_, error);
        }
    }

    axk::app::Result<void> publish(const Json &document) {
        std::error_code error;
        const auto parent = path_.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, error);
            if (error)
                return std::unexpected(
                    axk::app::Error{"connection_file_failed", "could not create the connection-file directory"});
        }

        std::filesystem::remove(path_, error);
        error.clear();
        auto temporary = path_;
        temporary += ".tmp." + std::to_string(process_id());
        std::filesystem::remove(temporary, error);
        error.clear();
        const auto contents = document.dump(2) + '\n';
        if (auto written = write_owner_only_file(temporary, contents); !written)
            return std::unexpected(written.error());
        std::filesystem::rename(temporary, path_, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return std::unexpected(axk::app::Error{"connection_file_failed", "could not publish the connection file"});
        }
        published_ = true;
        return {};
    }

  private:
    std::filesystem::path path_;
    bool published_{};
};

struct CorsMiddleware {
    struct context {};

    std::vector<std::string> allowed_origins;

    void before_handle(crow::request &request, crow::response &response, context &) {
        if (request.method != crow::HTTPMethod::Options)
            return;
        const auto origin = request.get_header_value("Origin");
        response.code =
            !origin.empty() && std::ranges::find(allowed_origins, origin) != allowed_origins.end() ? 204 : 403;
        response.end();
    }

    void after_handle(crow::request &request, crow::response &response, context &) {
        const auto origin = request.get_header_value("Origin");
        const auto allowed = !origin.empty() && std::ranges::find(allowed_origins, origin) != allowed_origins.end();
        if (request.method == crow::HTTPMethod::Options && !allowed) {
            response.code = 403;
            return;
        }
        if (allowed) {
            response.set_header("Access-Control-Allow-Origin", origin);
            response.set_header("Vary", "Origin");
            response.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
            response.set_header("Access-Control-Allow-Headers",
                                "Authorization, Content-Type, Idempotency-Key, X-Request-Id, Upload-Offset");
            response.set_header("Access-Control-Expose-Headers",
                                "Content-Length, Content-Range, Location, Upload-Offset, X-Request-Id");
        }
    }
};

struct RouteKey {
    axk::app::HttpMethod method;
    std::string route;

    friend bool operator<(const RouteKey &left, const RouteKey &right) {
        if (left.route != right.route)
            return left.route < right.route;
        return left.method < right.method;
    }
};

std::string next_request_id() {
    static std::atomic<std::uint64_t> sequence{1U};
    return "request-" + std::to_string(sequence.fetch_add(1U, std::memory_order_relaxed));
}

bool valid_request_id(std::string_view value) {
    return !value.empty() && value.size() <= 96U && std::ranges::all_of(value, [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '-' || character == '_' || character == '.';
    });
}

std::string request_id(const crow::request &request) {
    const auto supplied = request.get_header_value("X-Request-Id");
    return valid_request_id(supplied) ? supplied : next_request_id();
}

struct RequestTelemetryMiddleware {
    struct context {
        std::chrono::steady_clock::time_point started;
    };

    axk::server::RequestTelemetry *telemetry{};

    void before_handle(crow::request &, crow::response &, context &request_context) const {
        request_context.started = std::chrono::steady_clock::now();
        if (telemetry != nullptr)
            telemetry->begin_request();
    }

    void after_handle(crow::request &request, crow::response &response, context &request_context) const {
        if (telemetry == nullptr)
            return;
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                    request_context.started);
        telemetry->complete_request(response.code, duration);
        auto id = response.get_header_value("X-Request-Id");
        if (id.empty()) {
            id = request_id(request);
            response.set_header("X-Request-Id", id);
        }
        std::osyncstream{std::clog} << axk::server::structured_request_log(id, crow::method_name(request.method),
                                                                           request.url, response.code, duration)
                                    << '\n';
    }
};

bool constant_time_equal(std::string_view left, std::string_view right) {
    const auto size = std::max(left.size(), right.size());
    std::size_t difference = left.size() ^ right.size();
    for (std::size_t index = 0; index < size; ++index) {
        const auto left_byte = index < left.size() ? static_cast<unsigned char>(left[index]) : 0U;
        const auto right_byte = index < right.size() ? static_cast<unsigned char>(right[index]) : 0U;
        difference |= left_byte ^ right_byte;
    }
    return difference == 0U;
}

std::string token_digest(std::string_view token) {
    return axk::package_internal::hex_digest(
        axk::package_internal::sha256(std::as_bytes(std::span{token.data(), token.size()})));
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

int status_for_error(const axk::app::Error &error, int fallback = 422) {
    if (error.code == "request_too_large" || error.code == "json_structure_too_large")
        return 413;
    if (error.code == "job_not_found")
        return 404;
    if (error.code == "upload_not_found")
        return 404;
    if (error.code == "image_not_found")
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
    if (error.code == "download_archive_too_large")
        return 413;
    if (error.code == "upload_type_not_allowed")
        return 415;
    if (error.code == "invalid_upload_chunk" || error.code == "upload_not_ready")
        return 409;
    if (error.code == "upload_in_use" || error.code == "upload_materialization_failed")
        return 409;
    if (error.code == "download_archive_not_found")
        return 404;
    if (error.code == "idempotency_conflict")
        return 409;
    if (error.code == "destination_reserved")
        return 409;
    if (error.code == "output_exists")
        return 409;
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
    if (value == "audio")
        return axk::app::UploadKind::audio;
    if (value == "package")
        return axk::app::UploadKind::package;
    if (value == "manifest")
        return axk::app::UploadKind::manifest;
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

struct ByteRange {
    std::uint64_t offset{};
    std::uint64_t length{};
};

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

class ServerApplication {
  public:
    ServerApplication(axk::server::Config config, axk::app::OperationRegistry registry,
                      axk::server::WorkspaceStore workspaces)
        : config_(std::move(config)), workspaces_(std::move(workspaces)), sandbox_(workspaces_.sandbox()),
          uploads_(upload_directory(), config_.maximum_upload_total_bytes, config_.maximum_upload_bytes,
                   config_.maximum_uploads, config_.maximum_upload_chunk_bytes,
                   std::chrono::seconds{config_.upload_retention_seconds}),
          download_archives_(download_archive_directory(), config_.maximum_download_archive_total_bytes,
                             config_.maximum_download_archive_bytes, config_.maximum_download_archive_entries,
                             std::chrono::seconds{config_.download_archive_retention_seconds}),
          registry_(prepare_registry(std::move(registry), sandbox_, uploads_)),
          openapi_document_(axk::server::build_openapi_document(axk::server::embedded_openapi(), registry_)),
          openapi_validator_(openapi_document_),
          images_(sandbox_, config_.maximum_image_sessions, config_.maximum_page_size,
                  std::chrono::seconds{config_.image_idle_seconds}),
          jobs_(registry_, config_.job_worker_threads, config_.write_job_worker_threads, config_.maximum_queued_jobs,
                config_.replay_events_per_job, config_.maximum_retained_jobs,
                std::chrono::seconds{config_.job_retention_seconds}),
          event_tickets_(std::chrono::seconds{config_.event_ticket_ttl_seconds}, config_.maximum_event_tickets),
          event_dispatcher_(config_.maximum_websocket_delivery_events,
                            [this](const axk::app::JobEvent &event) { broadcast(event); }) {
        app_.template get_middleware<CorsMiddleware>().allowed_origins = config_.allowed_origins;
        app_.template get_middleware<RequestTelemetryMiddleware>().telemetry = &request_telemetry_;
        state_storage_ready_ =
            writable_directory(upload_directory()) && writable_directory(download_archive_directory());
        const auto publication_cleanup = sandbox_.cleanup_abandoned_publications();
        startup_cleanup_ready_ = publication_cleanup && state_storage_ready_ && cleanup_complete(upload_directory()) &&
                                 cleanup_complete(download_archive_directory());
        job_subscription_ = jobs_.subscribe(
            [this](const axk::app::JobEvent &event) { static_cast<void>(event_dispatcher_.publish(event)); });
        register_infrastructure_routes();
        register_event_route();
        register_operation_routes();
        app_.catchall_route()([this](const crow::request &request) {
            const auto id = request_id(request);
            if (request.method == crow::HTTPMethod::Options) {
                if (!origin_allowed(request))
                    return error_response(403, {"origin_denied", "request origin is not allowed"}, id);
                return crow::response{204};
            }
            return error_response(404, {"route_not_found", "API route does not exist"}, id);
        });
    }

    ~ServerApplication() {
        jobs_.unsubscribe(job_subscription_);
        jobs_.shutdown();
        event_dispatcher_.shutdown();
    }

    axk::app::Result<int> run() {
        app_.bindaddr(config_.bind_address)
            .port(config_.port)
            .concurrency(config_.worker_threads)
            .stream_threshold(config_.stream_threshold_bytes)
            .websocket_max_payload(config_.maximum_websocket_payload_bytes)
            .server_name("axklib-server");

        auto server = app_.run_async();
        if (app_.wait_for_server_start(std::chrono::seconds{5}) == std::cv_status::timeout) {
            app_.stop();
            server.wait();
            return std::unexpected(axk::app::Error{"server_start_failed", "Crow did not bind the configured endpoint"});
        }

        ScopedConnectionFile connection_file{config_.connection_file};
        if (!config_.connection_file.empty()) {
            const auto port = app_.port();
            const auto build = axk::current_build_info();
            const auto published = connection_file.publish(
                {{"schemaVersion", 1},
                 {"apiVersion", "v1"},
                 {"pid", process_id()},
                 {"baseUrl", "http://" + config_.bind_address + ":" + std::to_string(port) + "/api/v1"},
                 {"websocketUrl", "ws://" + config_.bind_address + ":" + std::to_string(port) + "/api/v1/events"},
                 {"bearerToken", config_.bearer_token},
                 {"semanticVersion", axk::version()},
                 {"sourceIdentity", build.source_identity}});
            if (!published) {
                app_.stop();
                server.wait();
                return std::unexpected(published.error());
            }
        }
        auto next_parent_check = std::chrono::steady_clock::now();
        while (server.wait_for(std::chrono::milliseconds{25}) != std::future_status::ready) {
            if (config_.parent_process_id != 0U && std::chrono::steady_clock::now() >= next_parent_check) {
                next_parent_check = std::chrono::steady_clock::now() + std::chrono::milliseconds{250};
                if (!axk::server::process_is_running(config_.parent_process_id)) {
                    std::clog << "axklib-server: owning desktop process exited; stopping\n";
                    app_.stop();
                    break;
                }
            }
            if (!shutdown_requested_.load(std::memory_order_acquire))
                continue;
            std::this_thread::sleep_for(std::chrono::milliseconds{25});
            app_.stop();
            break;
        }
        server.get();
        return 0;
    }

  private:
    static axk::app::OperationRegistry prepare_registry(axk::app::OperationRegistry registry,
                                                        const axk::app::Sandbox &sandbox,
                                                        axk::app::UploadStore &uploads) {
        auto prepared = axk::app::make_application_registry(sandbox, uploads, std::move(registry));
        if (!prepared)
            std::terminate();
        return std::move(*prepared);
    }

    struct EventClient {
        EventClient(std::size_t maximum_events, std::uint64_t maximum_bytes)
            : delivery_budget(maximum_events, maximum_bytes) {}

        std::mutex mutex;
        crow::websocket::connection *connection{};
        std::string owner_id;
        axk::server::EventDeliveryBudget delivery_budget;
    };

    using EventClientHandle = std::shared_ptr<EventClient>;

    [[nodiscard]] std::filesystem::path upload_directory() const {
        if (!config_.state_directory.empty())
            return config_.state_directory / "uploads";
        return std::filesystem::temp_directory_path() / "axklib-server" / "uploads";
    }

    [[nodiscard]] std::filesystem::path download_archive_directory() const {
        if (!config_.state_directory.empty())
            return config_.state_directory / "download-archives";
        return std::filesystem::temp_directory_path() / "axklib-server" / "download-archives";
    }

    std::optional<std::string> authenticated_principal(const crow::request &request) const {
        constexpr std::string_view prefix{"Bearer "};
        const auto header = request.get_header_value("Authorization");
        if (!header.starts_with(prefix))
            return std::nullopt;
        const auto token = std::string_view{header}.substr(prefix.size());
        if (!config_.bearer_token.empty() && constant_time_equal(token, config_.bearer_token))
            return "loopback";
        const auto digest = token_digest(token);
        for (const auto &configured : config_.token_hashes) {
            if (constant_time_equal(digest, configured.sha256))
                return configured.principal_id;
        }
        return std::nullopt;
    }

    std::string request_owner(const crow::request &request) const { return *authenticated_principal(request); }

    bool origin_allowed(const crow::request &request) const {
        const auto origin = request.get_header_value("Origin");
        return origin.empty() || std::ranges::find(config_.allowed_origins, origin) != config_.allowed_origins.end();
    }

    void audit(std::string_view id, std::string_view action, std::string_view outcome,
               std::string_view principal_id = {}, std::string_view resource_type = {},
               std::string_view resource_id = {}) const {
        std::osyncstream{std::clog} << axk::server::structured_audit_log(id, action, outcome, principal_id,
                                                                         resource_type, resource_id)
                                    << '\n';
    }

    std::optional<crow::response> guard(const crow::request &request, std::string_view id) const {
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

    crow::response preflight_response(const crow::request &request) const {
        return crow::response{origin_allowed(request) ? 204 : 403};
    }

    crow::response capability_response(const crow::request &request) const {
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
                {"operationClass",
                 entry.descriptor.operation_class == axk::app::OperationClass::read ? "READ" : "WRITE"},
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
                          {"maximumUploadBytes", config_.maximum_upload_bytes},
                          {"maximumUploadTotalBytes", config_.maximum_upload_total_bytes},
                          {"maximumUploadChunkBytes", config_.maximum_upload_chunk_bytes},
                          {"maximumDownloadRangeBytes", config_.maximum_download_range_bytes},
                          {"maximumDownloadArchiveBytes", config_.maximum_download_archive_bytes},
                          {"maximumDownloadArchiveTotalBytes", config_.maximum_download_archive_total_bytes},
                          {"maximumDownloadArchiveEntries", config_.maximum_download_archive_entries},
                          {"downloadArchiveRetentionSeconds", config_.download_archive_retention_seconds},
                          {"maximumWebsocketDeliveryEvents", config_.maximum_websocket_delivery_events},
                          {"maximumWebsocketDeliveryBytes", config_.maximum_websocket_delivery_bytes},
                          {"maximumQueuedJobs", config_.maximum_queued_jobs},
                          {"maximumImageSessions", config_.maximum_image_sessions},
                          {"maximumPageSize", config_.maximum_page_size}};
        return json_response(
            200,
            {{"data", {{"apiVersion", "v1"}, {"operations", std::move(operations)}, {"limits", limits}}},
             {"meta", {{"requestId", id}}}},
            id);
    }

    const Json &openapi_document() const noexcept { return openapi_document_; }

    crow::response roots_response(const crow::request &request) {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        Json roots = Json::array();
        static_cast<void>(workspaces_.snapshot());
        for (const auto &root : sandbox_.roots())
            roots.push_back({{"id", root.id}, {"displayName", root.display_name}, {"writable", root.writable}});
        return json_response(200, {{"data", {{"roots", std::move(roots)}}}, {"meta", {{"requestId", id}}}}, id);
    }

    Json workspace_json(const axk::server::WorkspaceInfo &workspace) const {
        return {{"id", workspace.definition.id},
                {"displayName", workspace.definition.display_name},
                {"path", axk::text::path_to_utf8(workspace.definition.path)},
                {"writable", workspace.definition.writable},
                {"effectiveWritable", workspace.effective_writable},
                {"status", axk::server::workspace_status_name(workspace.status)},
                {"issue", workspace.issue ? Json(*workspace.issue) : Json{}}};
    }

    crow::response workspace_snapshot_response(const crow::request &request) {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        const auto snapshot = workspaces_.snapshot();
        Json workspaces = Json::array();
        for (const auto &workspace : snapshot.workspaces)
            workspaces.push_back(workspace_json(workspace));
        return json_response(
            200,
            {{"data",
              {{"state", axk::server::workspace_configuration_state_name(snapshot.state)},
               {"revision", snapshot.revision},
               {"workspaces", std::move(workspaces)},
               {"configurationIssue", snapshot.configuration_issue ? Json(*snapshot.configuration_issue) : Json{}}}},
             {"meta", {{"requestId", id}}}},
            id);
    }

    crow::response workspace_create_response(const crow::request &request) {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        const auto parsed = parse_json_body(request, config_);
        if (!parsed)
            return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);
        try {
            auto path = axk::text::path_from_utf8(parsed->at("path").get<std::string>());
            if (!path)
                return error_response(400, {"invalid_request", "workspace path is not valid UTF-8"}, id);
            auto added = workspaces_.add(parsed->at("displayName").get<std::string>(), std::move(*path),
                                         parsed->value("writable", true), parsed->at("revision").get<std::uint64_t>());
            if (!added)
                return error_response(added.error().code == "workspace_revision_conflict" ? 409 : 422, added.error(),
                                      id);
            static_cast<void>(workspaces_.snapshot());
            return json_response(201, {{"data", workspace_json(*added)}, {"meta", {{"requestId", id}}}}, id);
        } catch (const std::exception &) {
            return error_response(400, {"invalid_request", "workspace fields do not match the schema"}, id);
        }
    }

    crow::response workspace_item_response(const crow::request &request, const std::string &workspace_id) {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        const auto parsed = parse_json_body(request, config_);
        if (!parsed)
            return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);
        try {
            const auto revision = parsed->at("revision").get<std::uint64_t>();
            if (images_.root_in_use(workspace_id) || jobs_.root_in_use(workspace_id)) {
                return error_response(
                    409, {"workspace_in_use", "close images and wait for active jobs before changing this workspace"},
                    id);
            }
            if (request.method == crow::HTTPMethod::Delete) {
                auto removed = workspaces_.remove(workspace_id, revision);
                if (!removed)
                    return error_response(removed.error().code == "workspace_revision_conflict" ? 409 : 404,
                                          removed.error(), id);
                static_cast<void>(workspaces_.snapshot());
                return crow::response{204};
            }
            std::optional<std::string> display_name;
            std::optional<std::filesystem::path> path;
            std::optional<bool> writable;
            if (const auto found = parsed->find("displayName"); found != parsed->end())
                display_name = found->get<std::string>();
            if (const auto found = parsed->find("path"); found != parsed->end()) {
                auto parsed_path = axk::text::path_from_utf8(found->get<std::string>());
                if (!parsed_path)
                    return error_response(400, {"invalid_request", "workspace path is not valid UTF-8"}, id);
                path = std::move(*parsed_path);
            }
            if (const auto found = parsed->find("writable"); found != parsed->end())
                writable = found->get<bool>();
            auto updated =
                workspaces_.update(workspace_id, std::move(display_name), std::move(path), writable, revision);
            if (!updated)
                return error_response(updated.error().code == "workspace_revision_conflict" ? 409 : 422,
                                      updated.error(), id);
            static_cast<void>(workspaces_.snapshot());
            return json_response(200, {{"data", workspace_json(*updated)}, {"meta", {{"requestId", id}}}}, id);
        } catch (const std::exception &) {
            return error_response(400, {"invalid_request", "workspace fields do not match the schema"}, id);
        }
    }

    crow::response workspace_reset_response(const crow::request &request) {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        auto reset = workspaces_.archive_and_reset();
        if (!reset)
            return error_response(500, reset.error(), id);
        return json_response(
            200,
            {{"data", {{"archivedPath", *reset ? Json(axk::text::path_to_utf8(**reset)) : Json{}}, {"revision", 0U}}},
             {"meta", {{"requestId", id}}}},
            id);
    }

    crow::response host_directory_roots_response(const crow::request &request) const {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        Json roots = Json::array();
#if defined(_WIN32)
        const auto drives = GetLogicalDrives();
        for (unsigned int index = 0U; index < 26U; ++index) {
            if ((drives & (1UL << index)) == 0U)
                continue;
            const auto letter = static_cast<char>('A' + index);
            const auto path = std::string{letter} + ":/";
            roots.push_back({{"name", path}, {"path", path}});
        }
#else
        roots.push_back({{"name", "Filesystem"}, {"path", "/"}});
#endif
        return json_response(200, {{"data", {{"roots", std::move(roots)}}}, {"meta", {{"requestId", id}}}}, id);
    }

    crow::response host_directory_listing_response(const crow::request &request) const {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        const auto parsed = parse_json_body(request, config_);
        if (!parsed)
            return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);
        try {
            auto parsed_path = axk::text::path_from_utf8(parsed->at("path").get<std::string>());
            if (!parsed_path)
                return error_response(400, {"invalid_request", "host directory path is not valid UTF-8"}, id);
            auto path = std::move(*parsed_path);
            const auto limit = static_cast<std::size_t>(std::min(parsed->value("limit", 200U), 500U));
            const auto offset = parsed->value("cursor", std::string{});
            std::size_t first{};
            if (!offset.empty()) {
                const auto [tail, error] = std::from_chars(offset.data(), offset.data() + offset.size(), first);
                if (error != std::errc{} || tail != offset.data() + offset.size())
                    return error_response(400, {"invalid_cursor", "directory cursor is invalid"}, id);
            }
            if (!path.is_absolute())
                return error_response(422, {"invalid_host_directory", "host directory path must be absolute"}, id);
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
                return error_response(422, {"invalid_host_directory", "host directory is inaccessible"}, id);
            path = std::filesystem::canonical(path, error);
            if (error)
                return error_response(422, {"invalid_host_directory", "host directory is inaccessible"}, id);
            std::vector<std::filesystem::path> directories;
            for (std::filesystem::directory_iterator
                     iterator{path, std::filesystem::directory_options::skip_permission_denied, error},
                 end;
                 !error && iterator != end; iterator.increment(error)) {
                std::error_code entry_error;
                const auto entry_status = iterator->symlink_status(entry_error);
                if (!entry_error && !std::filesystem::is_symlink(entry_status) &&
                    std::filesystem::is_directory(entry_status)) {
                    directories.push_back(iterator->path());
                }
            }
            if (error)
                return error_response(422, {"invalid_host_directory", "host directory cannot be listed"}, id);
            std::ranges::sort(directories, {},
                              [](const auto &entry) { return axk::text::path_to_utf8(entry.filename()); });
            if (first > directories.size())
                return error_response(400, {"invalid_cursor", "directory cursor is outside the listing"}, id);
            Json entries = Json::array();
            const auto end = first + std::min(limit, directories.size() - first);
            for (auto index = first; index < end; ++index) {
                entries.push_back({{"name", axk::text::path_to_utf8(directories[index].filename())},
                                   {"path", axk::text::path_to_utf8(directories[index])}});
            }
            return json_response(200,
                                 {{"data",
                                   {{"path", axk::text::path_to_utf8(path)},
                                    {"parentPath", path.has_parent_path() && path != path.root_path()
                                                       ? Json(axk::text::path_to_utf8(path.parent_path()))
                                                       : Json{}},
                                    {"entries", std::move(entries)},
                                    {"nextCursor", end < directories.size() ? Json(std::to_string(end)) : Json{}}}},
                                  {"meta", {{"requestId", id}}}},
                                 id);
        } catch (const std::exception &) {
            return error_response(400, {"invalid_request", "host directory fields do not match the schema"}, id);
        }
    }

    crow::response directory_listing_response(const crow::request &request) const {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        const auto parsed = parse_json_body(request, config_);
        if (!parsed)
            return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);
        const auto &input = *parsed;

        axk::app::DirectoryRef directory;
        std::size_t limit = 200U;
        std::optional<std::string> cursor;
        try {
            const auto &reference = input.at("directory");
            directory.root_id = reference.at("rootId").get<std::string>();
            directory.relative_path = reference.at("relativePath").get<std::string>();
            if (const auto found = input.find("limit"); found != input.end())
                limit = found->get<std::size_t>();
            if (const auto found = input.find("cursor"); found != input.end() && !found->is_null())
                cursor = found->get<std::string>();
        } catch (const Json::exception &) {
            return error_response(400, {"invalid_request", "directory, limit, and cursor do not match the schema"}, id);
        }
        if (cursor && (cursor->empty() || cursor->size() > maximum_cursor_length))
            return error_response(400, {"invalid_cursor", "cursor length is outside the configured contract"}, id);
        const auto listing = sandbox_.list_directory(directory, limit, cursor);
        if (!listing)
            return error_response(422, listing.error(), id);
        Json entries = Json::array();
        for (const auto &entry : listing->entries) {
            entries.push_back({{"name", entry.name},
                               {"relativePath", entry.relative_path},
                               {"kind", axk::app::directory_entry_kind_name(entry.kind)},
                               {"size", entry.size ? Json(*entry.size) : Json{}}});
        }
        return json_response(
            200,
            {{"data",
              {{"directory",
                {{"rootId", listing->directory.root_id}, {"relativePath", listing->directory.relative_path}}},
               {"entries", std::move(entries)},
               {"truncated", listing->truncated},
               {"nextCursor", listing->next_cursor ? Json(*listing->next_cursor) : Json{}}}},
             {"meta", {{"requestId", id}}}},
            id);
    }

    crow::response metadata_response(const crow::request &request) const {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        const auto parsed = parse_json_body(request, config_);
        if (!parsed)
            return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);
        const auto &input = *parsed;
        std::string root_id;
        std::string relative_path;
        try {
            root_id = input.at("rootId").get<std::string>();
            relative_path = input.at("relativePath").get<std::string>();
        } catch (const Json::exception &) {
            return error_response(400, {"invalid_request", "rootId and relativePath are required strings"}, id);
        }
        const auto metadata = sandbox_.metadata(root_id, relative_path);
        if (!metadata)
            return error_response(422, metadata.error(), id);
        return json_response(200,
                             {{"data",
                               {{"rootId", metadata->root_id},
                                {"relativePath", metadata->relative_path},
                                {"kind", axk::app::directory_entry_kind_name(metadata->kind)},
                                {"size", metadata->size ? Json(*metadata->size) : Json{}},
                                {"writable", metadata->writable}}},
                              {"meta", {{"requestId", id}}}},
                             id);
    }

    Json image_summary_json(const axk::app::ImageSessionSummary &summary) const {
        return {{"imageId", summary.image_id},
                {"source", {{"rootId", summary.source.root_id}, {"relativePath", summary.source.relative_path}}},
                {"format", summary.format},
                {"availableOperations", summary.available_operations},
                {"rootCount", summary.root_count},
                {"objectCount", summary.object_count},
                {"relationshipCount", summary.relationship_count},
                {"validation",
                 {{"valid", summary.validation.valid()},
                  {"infoCount", summary.validation.info_count},
                  {"warningCount", summary.validation.warning_count},
                  {"errorCount", summary.validation.error_count}}}};
    }

    crow::response create_image_response(const crow::request &request) {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        const auto parsed = parse_json_body(request, config_);
        if (!parsed)
            return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);
        const auto &input = *parsed;
        axk::app::FileRef source;
        try {
            const auto &reference = input.at("source");
            source.root_id = reference.at("rootId").get<std::string>();
            source.relative_path = reference.at("relativePath").get<std::string>();
        } catch (const Json::exception &) {
            return error_response(400, {"invalid_request", "source must be one sandbox FileRef"}, id);
        }
        const auto opened = images_.open(source, request_owner(request));
        if (!opened)
            return error_response(status_for_error(opened.error()), opened.error(), id);
        auto response = json_response(201, {{"data", image_summary_json(*opened)}, {"meta", {{"requestId", id}}}}, id);
        response.set_header("Location", "/api/v1/images/" + opened->image_id);
        return response;
    }

    crow::response image_response(const crow::request &request, const std::string &image_id) {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        if (request.method == crow::HTTPMethod::Delete) {
            const auto closed = images_.close(image_id, request_owner(request));
            if (!closed)
                return error_response(status_for_error(closed.error()), closed.error(), id);
            return json_response(200, {{"data", {{"closed", true}}}, {"meta", {{"requestId", id}}}}, id);
        }
        const auto summary = images_.inspect(image_id, request_owner(request));
        if (!summary)
            return error_response(status_for_error(summary.error()), summary.error(), id);
        return json_response(200, {{"data", image_summary_json(*summary)}, {"meta", {{"requestId", id}}}}, id);
    }

    template <typename Item, typename Loader, typename Serializer>
    crow::response image_page_response(const crow::request &request, const std::string &image_id, Loader loader,
                                       Serializer serializer) {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        std::size_t limit = std::min<std::size_t>(200U, config_.maximum_page_size);
        if (const auto *value = request.url_params.get("limit"); value != nullptr) {
            const auto parsed = parse_unsigned(value);
            if (!parsed || *parsed > std::numeric_limits<std::size_t>::max())
                return error_response(400, {"invalid_page", "limit must be an unsigned integer"}, id);
            limit = static_cast<std::size_t>(*parsed);
        }
        std::optional<std::string_view> cursor;
        if (const auto *value = request.url_params.get("cursor"); value != nullptr && *value != '\0') {
            cursor = value;
            if (cursor->size() > maximum_cursor_length)
                return error_response(400, {"invalid_cursor", "cursor length is outside the configured contract"}, id);
        }
        const auto page = loader(image_id, request_owner(request), limit, cursor);
        if (!page)
            return error_response(status_for_error(page.error()), page.error(), id);
        Json items = Json::array();
        for (const Item &item : page->items)
            items.push_back(serializer(item));
        return json_response(200,
                             {{"data",
                               {{"items", std::move(items)},
                                {"totalCount", page->total_count},
                                {"nextCursor", page->next_cursor ? Json(*page->next_cursor) : Json{}}}},
                              {"meta", {{"requestId", id}}}},
                             id);
    }

    crow::response image_content_response(const crow::request &request, const std::string &image_id) {
        std::optional<std::string_view> parent_id;
        if (const auto *value = request.url_params.get("parentId"); value != nullptr && *value != '\0')
            parent_id = value;
        return image_page_response<axk::app::ImageContentItem>(
            request, image_id,
            [this, parent_id](auto id, auto owner, auto limit, auto cursor) {
                return images_.content(id, owner, limit, cursor, parent_id);
            },
            [](const axk::app::ImageContentItem &item) {
                return Json{{"id", item.id},
                            {"parentId", item.parent_id ? Json(*item.parent_id) : Json{}},
                            {"depth", item.depth},
                            {"kind", item.kind},
                            {"displayName", item.display_name},
                            {"childCount", item.child_count},
                            {"objectId", item.object_id ? Json(*item.object_id) : Json{}},
                            {"objectType", item.object_type ? Json(*item.object_type) : Json{}},
                            {"quality", item.quality},
                            {"basis", item.basis},
                            {"notes", item.notes},
                            {"details", item.details}};
            });
    }

    crow::response image_objects_response(const crow::request &request, const std::string &image_id) {
        std::optional<std::string_view> object_type;
        if (const auto *value = request.url_params.get("type"); value != nullptr && *value != '\0')
            object_type = value;
        std::optional<std::string_view> content_scope_id;
        if (const auto *value = request.url_params.get("scopeId"); value != nullptr && *value != '\0')
            content_scope_id = value;
        return image_page_response<axk::app::ImageObjectItem>(
            request, image_id,
            [this, object_type, content_scope_id](auto id, auto owner, auto limit, auto cursor) {
                return images_.objects(id, owner, limit, cursor, object_type, content_scope_id);
            },
            [](const axk::app::ImageObjectItem &item) {
                Json waveform;
                if (item.waveform) {
                    waveform = {{"sampleRate", item.waveform->sample_rate},
                                {"sampleWidthBytes", item.waveform->sample_width_bytes},
                                {"rootKey", item.waveform->root_key},
                                {"fineTuneCents", item.waveform->fine_tune_cents},
                                {"loopMode", item.waveform->loop_mode},
                                {"loopModeLabel", item.waveform->loop_mode_label},
                                {"frameCount", item.waveform->frame_count},
                                {"loopStartFrame", item.waveform->loop_start_frame},
                                {"loopLengthFrames", item.waveform->loop_length_frames}};
                }
                return Json{{"id", item.id},
                            {"type", item.type},
                            {"name", item.name},
                            {"format", item.format},
                            {"partitionIndex", item.partition_index ? Json(*item.partition_index) : Json{}},
                            {"partitionName", item.partition_name},
                            {"volumeName", item.volume_name},
                            {"categoryName", item.category_name},
                            {"entryName", item.entry_name},
                            {"sizeBytes", item.stored_size_bytes},
                            {"waveform", std::move(waveform)}};
            });
    }

    crow::response image_relationships_response(const crow::request &request, const std::string &image_id) {
        axk::app::ImageRelationshipFilter filter;
        if (const auto *value = request.url_params.get("scopeId"); value != nullptr && *value != '\0')
            filter.content_scope_id = value;
        if (const auto *value = request.url_params.get("sourceObjectId"); value != nullptr && *value != '\0')
            filter.source_object_id = value;
        if (const auto *value = request.url_params.get("targetObjectId"); value != nullptr && *value != '\0')
            filter.target_object_id = value;
        if (const auto *value = request.url_params.get("type"); value != nullptr && *value != '\0')
            filter.relationship_type = value;
        return image_page_response<axk::app::ImageRelationshipItem>(
            request, image_id,
            [this, filter](auto id, auto owner, auto limit, auto cursor) {
                return images_.relationships(id, owner, limit, cursor, filter);
            },
            [](const axk::app::ImageRelationshipItem &item) {
                return Json{{"id", item.id},
                            {"sourceObjectId", item.source_object_id},
                            {"targetObjectId", item.target_object_id ? Json(*item.target_object_id) : Json{}},
                            {"candidateObjectIds", item.candidate_object_ids},
                            {"type", item.type},
                            {"quality", item.quality},
                            {"basis", item.basis},
                            {"notes", item.notes},
                            {"assignmentIndex", item.assignment_index ? Json(*item.assignment_index) : Json{}},
                            {"assignmentName", item.assignment_name},
                            {"assignmentState", item.assignment_state},
                            {"receiveChannelDisplay", item.receive_channel_display}};
            });
    }

    crow::response image_validation_response(const crow::request &request, const std::string &image_id) {
        return image_page_response<axk::app::ImageValidationItem>(
            request, image_id,
            [this](auto id, auto owner, auto limit, auto cursor) {
                return images_.validation_issues(id, owner, limit, cursor);
            },
            [](const axk::app::ImageValidationItem &item) {
                return Json{{"code", item.code},
                            {"severity", item.severity},
                            {"message", item.message},
                            {"samplerPath", item.sampler_path},
                            {"objectId", item.object_id ? Json(*item.object_id) : Json{}}};
            });
    }

    crow::response image_preview_response(const crow::request &request, const std::string &image_id) {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        const auto *object_id = request.url_params.get("objectId");
        const auto bins =
            parse_unsigned(request.url_params.get("bins") == nullptr ? "" : request.url_params.get("bins"));
        if (object_id == nullptr || *object_id == '\0' || !bins || *bins > std::numeric_limits<std::size_t>::max())
            return error_response(400, {"invalid_preview", "objectId and an unsigned bins value are required"}, id);
        const auto preview =
            images_.preview(image_id, request_owner(request), object_id, static_cast<std::size_t>(*bins));
        if (!preview)
            return error_response(status_for_error(preview.error()), preview.error(), id);
        Json values = Json::array();
        for (const auto &bin : preview->bins)
            values.push_back({{"minimum", bin.minimum}, {"maximum", bin.maximum}});
        return json_response(
            200,
            {{"data",
              {{"objectId", preview->object_id}, {"frameCount", preview->frame_count}, {"bins", std::move(values)}}},
             {"meta", {{"requestId", id}}}},
            id);
    }

    crow::response create_upload_response(const crow::request &request) {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        const auto parsed = parse_json_body(request, config_);
        if (!parsed)
            return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);
        const auto &input = *parsed;
        axk::app::UploadCreateRequest create_request;
        try {
            create_request.owner_id = request_owner(request);
            create_request.filename = input.at("filename").get<std::string>();
            const auto kind = parse_upload_kind(input.at("kind").get<std::string>());
            if (!kind)
                return error_response(400, {"invalid_request", "upload kind must be audio, package, or manifest"}, id);
            create_request.kind = *kind;
            create_request.media_type = input.at("mediaType").get<std::string>();
            create_request.declared_size = input.at("size").get<std::uint64_t>();
            if (const auto digest = input.find("sha256"); digest != input.end() && !digest->is_null())
                create_request.sha256 = digest->get<std::string>();
        } catch (const Json::exception &) {
            return error_response(400, {"invalid_request", "upload metadata does not match the schema"}, id);
        }
        const auto upload = uploads_.create(std::move(create_request));
        if (!upload) {
            audit(id, "upload_create", "denied", request_owner(request));
            return error_response(status_for_error(upload.error()), upload.error(), id);
        }
        audit(id, "upload_create", "allowed", request_owner(request), "upload", upload->reference.upload_id);
        auto response = json_response(201, {{"data", upload_json(*upload)}, {"meta", {{"requestId", id}}}}, id);
        response.set_header("Location", "/api/v1/uploads/" + upload->reference.upload_id);
        response.set_header("Upload-Offset", "0");
        return response;
    }

    crow::response upload_response(const crow::request &request, const std::string &upload_id) {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        const axk::app::UploadRef reference{upload_id};
        if (request.method == crow::HTTPMethod::Delete) {
            const auto removed = uploads_.remove(reference, request_owner(request));
            if (!removed) {
                audit(id, "upload_delete", "denied", request_owner(request), "upload", upload_id);
                return error_response(status_for_error(removed.error()), removed.error(), id);
            }
            audit(id, "upload_delete", "allowed", request_owner(request), "upload", upload_id);
            return crow::response{204};
        }
        if (request.method == crow::HTTPMethod::Put) {
            const auto offset = parse_unsigned(request.get_header_value("Upload-Offset"));
            if (!offset)
                return error_response(400, {"invalid_upload_chunk", "Upload-Offset must be an unsigned integer"}, id);
            if (request.body.size() > uploads_.maximum_chunk_bytes())
                return error_response(413, {"request_too_large", "upload chunk exceeds the configured limit"}, id);
            const auto body = std::as_bytes(std::span{request.body.data(), request.body.size()});
            const auto upload = uploads_.append(reference, request_owner(request), *offset, body);
            if (!upload) {
                audit(id, "upload_append", "denied", request_owner(request), "upload", upload_id);
                return error_response(status_for_error(upload.error()), upload.error(), id);
            }
            audit(id, "upload_append", "allowed", request_owner(request), "upload", upload_id);
            auto response = json_response(200, {{"data", upload_json(*upload)}, {"meta", {{"requestId", id}}}}, id);
            response.set_header("Upload-Offset", std::to_string(upload->received_size));
            return response;
        }
        const auto upload = uploads_.inspect(reference, request_owner(request));
        if (!upload)
            return error_response(status_for_error(upload.error()), upload.error(), id);
        auto response = json_response(200, {{"data", upload_json(*upload)}, {"meta", {{"requestId", id}}}}, id);
        response.set_header("Upload-Offset", std::to_string(upload->received_size));
        return response;
    }

    crow::response complete_upload_response(const crow::request &request, const std::string &upload_id) {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        const auto upload = uploads_.complete({upload_id}, request_owner(request));
        if (!upload) {
            audit(id, "upload_complete", "denied", request_owner(request), "upload", upload_id);
            return error_response(status_for_error(upload.error()), upload.error(), id);
        }
        audit(id, "upload_complete", "allowed", request_owner(request), "upload", upload_id);
        return json_response(200, {{"data", upload_json(*upload)}, {"meta", {{"requestId", id}}}}, id);
    }

    crow::response materialize_upload_response(const crow::request &request, const std::string &upload_id) {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        const auto parsed = parse_json_body(request, config_);
        if (!parsed)
            return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);
        const auto &input = *parsed;
        axk::app::FileRef destination;
        bool overwrite{};
        try {
            const auto &reference = input.at("destination");
            destination.root_id = reference.at("rootId").get<std::string>();
            destination.relative_path = reference.at("relativePath").get<std::string>();
            overwrite = input.value("overwrite", false);
        } catch (const Json::exception &) {
            return error_response(400, {"invalid_request", "destination must be one sandbox FileRef"}, id);
        }
        const auto materialized =
            uploads_.materialize({upload_id}, request_owner(request), sandbox_, destination, overwrite);
        if (!materialized) {
            audit(id, "upload_materialize", "denied", request_owner(request), "upload", upload_id);
            return error_response(status_for_error(materialized.error()), materialized.error(), id);
        }
        audit(id, "upload_materialize", "allowed", request_owner(request), "upload", upload_id);
        return json_response(
            201,
            {{"data", {{"file", {{"rootId", materialized->root_id}, {"relativePath", materialized->relative_path}}}}},
             {"meta", {{"requestId", id}}}},
            id);
    }

    crow::response download_response(const crow::request &request) const {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        const auto *root_id = request.url_params.get("rootId");
        const auto *relative_path = request.url_params.get("relativePath");
        if (root_id == nullptr || relative_path == nullptr)
            return error_response(400, {"invalid_request", "rootId and relativePath query parameters are required"},
                                  id);
        const auto resolved = sandbox_.resolve_file({root_id, relative_path});
        if (!resolved) {
            audit(id, "file_download", "denied", request_owner(request), "root", root_id);
            return error_response(404, resolved.error(), id);
        }
        std::error_code error;
        const auto size = std::filesystem::file_size(*resolved, error);
        if (error)
            return error_response(422, {"file_unavailable", "sandbox file size cannot be read"}, id);

        crow::response response;
        const auto range_header = request.get_header_value("Range");
        if (range_header.empty()) {
            response.set_static_file_info_unsafe(resolved->string());
        } else {
            const auto range = parse_byte_range(range_header, size, config_.maximum_download_range_bytes);
            if (!range) {
                response = error_response(416, {"invalid_range", "requested byte range is invalid or too large"}, id);
                response.set_header("Content-Range", "bytes */" + std::to_string(size));
                return response;
            }
            std::ifstream input{*resolved, std::ios::binary};
            input.seekg(static_cast<std::streamoff>(range->offset));
            response.body.resize(static_cast<std::size_t>(range->length));
            input.read(response.body.data(), static_cast<std::streamsize>(range->length));
            if (!input)
                return error_response(422, {"file_unavailable", "sandbox byte range cannot be read"}, id);
            response.code = 206;
            response.set_header("Content-Type", "application/octet-stream");
            response.set_header("Content-Range", "bytes " + std::to_string(range->offset) + "-" +
                                                     std::to_string(range->offset + range->length - 1U) + "/" +
                                                     std::to_string(size));
        }
        response.set_header("X-Request-Id", id);
        response.set_header("Cache-Control", "no-store");
        response.set_header("Accept-Ranges", "bytes");
        response.set_header("Content-Disposition", "attachment; filename=\"" + resolved->filename().string() + "\"");
        audit(id, "file_download", "allowed", request_owner(request), "root", root_id);
        return response;
    }

    crow::response create_download_archive_response(const crow::request &request) {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        const auto parsed = parse_json_body(request, config_);
        if (!parsed)
            return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);
        axk::app::DirectoryRef directory;
        try {
            const auto &value = parsed->at("directory");
            directory = {value.at("rootId").get<std::string>(), value.at("relativePath").get<std::string>()};
        } catch (const Json::exception &) {
            return error_response(400, {"invalid_request", "directory must be one sandbox DirectoryRef"}, id);
        }
        const auto archive = download_archives_.create(request_owner(request), sandbox_, directory);
        if (!archive) {
            audit(id, "archive_create", "denied", request_owner(request), "root", directory.root_id);
            return error_response(status_for_error(archive.error()), archive.error(), id);
        }
        audit(id, "archive_create", "allowed", request_owner(request), "archive", archive->reference.archive_id);
        const auto content_path = "/api/v1/download-archives/" + archive->reference.archive_id + "/content";
        auto response = json_response(201,
                                      {{"data",
                                        {{"archiveId", archive->reference.archive_id},
                                         {"filename", archive->filename},
                                         {"sizeBytes", archive->size_bytes},
                                         {"entryCount", archive->entry_count},
                                         {"expiresInSeconds", archive->expires_in_seconds},
                                         {"contentPath", content_path}}},
                                       {"meta", {{"requestId", id}}}},
                                      id);
        response.set_header("Location", content_path);
        return response;
    }

    crow::response download_archive_response(const crow::request &request, const std::string &archive_id) {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        const axk::app::DownloadArchiveRef reference{archive_id};
        if (request.method == crow::HTTPMethod::Delete) {
            const auto removed = download_archives_.remove(reference, request_owner(request));
            if (!removed) {
                audit(id, "archive_delete", "denied", request_owner(request), "archive", archive_id);
                return error_response(status_for_error(removed.error()), removed.error(), id);
            }
            audit(id, "archive_delete", "allowed", request_owner(request), "archive", archive_id);
            return crow::response{204};
        }
        const auto snapshot = download_archives_.inspect(reference, request_owner(request));
        if (!snapshot)
            return error_response(status_for_error(snapshot.error()), snapshot.error(), id);
        const auto path = download_archives_.resolve(reference, request_owner(request));
        if (!path)
            return error_response(status_for_error(path.error()), path.error(), id);
        crow::response response;
        response.set_static_file_info_unsafe(path->string());
        response.set_header("X-Request-Id", id);
        response.set_header("Cache-Control", "no-store");
        response.set_header("Content-Type", "application/x-tar");
        response.set_header("Content-Disposition", "attachment; filename=\"" + snapshot->filename + "\"");
        audit(id, "archive_download", "allowed", request_owner(request), "archive", archive_id);
        return response;
    }

    axk::app::Result<Json> wire_job_snapshot(const axk::app::JobSnapshot &snapshot) const {
        auto result = axk::server::job_snapshot_json(snapshot);
        if (snapshot.result) {
            const auto *descriptor = registry_.find(snapshot.operation_id);
            if (descriptor == nullptr)
                return std::unexpected(axk::app::Error{"response_contract_error", "job operation metadata is missing"});
            auto wire_result = openapi_validator_.wire_value(descriptor->result_schema, *snapshot.result);
            if (const auto valid = openapi_validator_.validate(descriptor->result_schema, wire_result); !valid) {
                return std::unexpected(
                    axk::app::Error{"response_contract_error", "job result violated its declared schema"});
            }
            result["result"] = std::move(wire_result);
        }
        if (const auto valid = openapi_validator_.validate("Job", result); !valid)
            return std::unexpected(axk::app::Error{"response_contract_error", "job snapshot violated its schema"});
        return result;
    }

    crow::response operation_response(const crow::request &request, const std::vector<std::string> &operation_ids) {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        Json input = Json::object();
        if (!request.body.empty()) {
            auto parsed = parse_json_body(request, config_);
            if (!parsed)
                return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);
            input = std::move(*parsed);
        }

        std::string selected;
        if (operation_ids.size() == 1U) {
            selected = operation_ids.front();
        } else {
            const auto operation = input.find("operationId");
            if (operation == input.end() || !operation->is_string()) {
                return error_response(400, {"operation_id_required", "operationId selects an operation on this route"},
                                      id);
            }
            selected = operation->get<std::string>();
            input.erase(operation);
            if (std::ranges::find(operation_ids, selected) == operation_ids.end())
                return error_response(400, {"invalid_operation_id", "operationId is not valid for this route"}, id);
        }

        const auto descriptor = registry_.find(selected);
        if (descriptor == nullptr)
            return error_response(500, {"contract_error", "operation metadata is not available"}, id);
        if (const auto valid = openapi_validator_.validate(descriptor->request_schema, input); !valid)
            return error_response(400, valid.error(), id);
        input = openapi_validator_.application_value(descriptor->request_schema, input);
        if (descriptor->mode == axk::app::ExecutionMode::job) {
            std::optional<std::string> idempotency_key;
            const auto supplied_key = request.get_header_value("Idempotency-Key");
            if (!supplied_key.empty())
                idempotency_key = supplied_key;
            const auto submitted = jobs_.submit(selected, std::move(input),
                                                {.owner_id = request_owner(request),
                                                 .request_id = id,
                                                 .cancellation = {},
                                                 .progress = nullptr,
                                                 .display_path = {}},
                                                std::move(idempotency_key));
            if (!submitted)
                return error_response(status_for_error(submitted.error()), submitted.error(), id);
            audit(id, "job_submit", "allowed", request_owner(request), "job", submitted->job_id);
            auto snapshot = wire_job_snapshot(*submitted);
            if (!snapshot)
                return error_response(500, snapshot.error(), id);
            auto response = json_response(202, {{"data", std::move(*snapshot)}, {"meta", {{"requestId", id}}}}, id);
            response.set_header("Location", "/api/v1/jobs/" + submitted->job_id);
            return response;
        }
        const axk::app::OperationContext context{.owner_id = request_owner(request),
                                                 .request_id = id,
                                                 .cancellation = {},
                                                 .progress = nullptr,
                                                 .display_path = {}};
        const auto result = registry_.invoke(selected, input, context);
        if (!result) {
            return error_response(status_for_error(result.error()), result.error(), id);
        }
        auto wire_result = openapi_validator_.wire_value(descriptor->result_schema, *result);
        if (const auto valid = openapi_validator_.validate(descriptor->result_schema, wire_result); !valid)
            return error_response(500, {"response_contract_error", "operation result violated its declared schema"},
                                  id);
        return json_response(200, {{"data", std::move(wire_result)}, {"meta", {{"requestId", id}}}}, id);
    }

    crow::response job_response(const crow::request &request, const std::string &job_id) {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        if (request.method == crow::HTTPMethod::Delete) {
            if (const auto cancelled = jobs_.cancel(job_id, request_owner(request)); !cancelled) {
                audit(id, "job_cancel", "denied", request_owner(request), "job", job_id);
                return error_response(status_for_error(cancelled.error()), cancelled.error(), id);
            }
            audit(id, "job_cancel", "allowed", request_owner(request), "job", job_id);
        }
        const auto snapshot = jobs_.status(job_id, request_owner(request));
        if (!snapshot)
            return error_response(status_for_error(snapshot.error()), snapshot.error(), id);
        auto wire_snapshot = wire_job_snapshot(*snapshot);
        if (!wire_snapshot)
            return error_response(500, wire_snapshot.error(), id);
        return json_response(200, {{"data", std::move(*wire_snapshot)}, {"meta", {{"requestId", id}}}}, id);
    }

    crow::response job_events_response(const crow::request &request, const std::string &job_id) const {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        const auto after_sequence = parse_sequence(request.url_params.get("afterSequence"));
        if (!after_sequence)
            return error_response(400, {"invalid_cursor", "afterSequence must be an unsigned integer"}, id);
        const auto replay = jobs_.replay(job_id, request_owner(request), *after_sequence);
        if (!replay)
            return error_response(status_for_error(replay.error()), replay.error(), id);
        Json events = Json::array();
        for (const auto &event : *replay)
            events.push_back(axk::server::job_event_json(event));
        return json_response(200,
                             {{"data", {{"events", std::move(events)}}},
                              {"meta", {{"requestId", id}, {"afterSequence", *after_sequence}}}},
                             id);
    }

    crow::response event_ticket_response(const crow::request &request) {
        const auto id = request_id(request);
        if (auto denied = guard(request, id))
            return std::move(*denied);
        const auto ticket = event_tickets_.issue(request_owner(request));
        if (!ticket)
            return error_response(status_for_error(ticket.error()), ticket.error(), id);
        return json_response(201,
                             {{"data",
                               {{"ticket", ticket->ticket_id},
                                {"expiresInSeconds", ticket->expires_in_seconds},
                                {"websocketUrl", "/api/v1/events"},
                                {"subprotocol", event_subprotocol}}},
                              {"meta", {{"requestId", id}}}},
                             id);
    }

    void broadcast(const axk::app::JobEvent &event) {
        const auto message = axk::server::job_event_json(event).dump();
        std::vector<EventClientHandle> clients;
        {
            const std::scoped_lock lock{event_clients_mutex_};
            clients = event_clients_;
        }
        for (const auto &client : clients) {
            const std::scoped_lock lock{client->mutex};
            if (client->owner_id != event.owner_id || client->connection == nullptr)
                continue;
            if (!client->delivery_budget.admit(message.size())) {
                auto *connection = client->connection;
                client->connection = nullptr;
                websocket_clients_evicted_.fetch_add(1U, std::memory_order_relaxed);
                connection->close("event delivery budget exhausted; reconnect and replay", 1013U);
                continue;
            }
            client->connection->send_text(message);
        }
    }

    void register_infrastructure_routes() {
        app_.route_dynamic("/api/v1/system/health/live")([] { return crow::response{204}; });
        app_.route_dynamic("/api/v1/system/health/ready")([this](const crow::request &request) {
            const auto id = request_id(request);
            if (auto denied = guard(request, id))
                return std::move(*denied);
            const auto executor_ready = !shutdown_requested_.load(std::memory_order_relaxed);
            const auto ready = state_storage_ready_ && startup_cleanup_ready_ && executor_ready;
            const auto workspace_snapshot = workspaces_.snapshot();
            const auto state = [](bool value) { return value ? "READY" : "NOT_READY"; };
            return json_response(ready ? 200 : 503,
                                 {{"data",
                                   {{"ready", ready},
                                    {"checks",
                                     {{"configuration", "READY"},
                                      {"sandbox", "READY"},
                                      {"workspaceConfiguration",
                                       axk::server::workspace_configuration_state_name(workspace_snapshot.state)},
                                      {"stateStorage", state(state_storage_ready_)},
                                      {"startupCleanup", state(startup_cleanup_ready_)},
                                      {"executorAdmission", state(executor_ready)}}}}}},
                                 id);
        });
        app_.route_dynamic("/api/v1/system/capabilities")(
            [this](const crow::request &request) { return capability_response(request); });
        app_.route_dynamic("/api/v1/system/metrics")([this](const crow::request &request) {
            const auto id = request_id(request);
            if (auto denied = guard(request, id))
                return std::move(*denied);
            const auto metrics = request_telemetry_.snapshot();
            const auto job_metrics = jobs_.metrics();
            const auto event_metrics = event_dispatcher_.snapshot();
            return json_response(
                200,
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
                   {"websocketEventsDropped", event_metrics.dropped_events},
                   {"websocketEventsPending", event_metrics.pending_events},
                   {"websocketClientsEvicted", websocket_clients_evicted_.load(std::memory_order_relaxed)}}},
                 {"meta", {{"requestId", id}}}},
                id);
        });
        app_.route_dynamic("/api/v1/system/shutdown")
            .methods(crow::HTTPMethod::Post)([this](const crow::request &request) {
                const auto id = request_id(request);
                if (auto denied = guard(request, id))
                    return std::move(*denied);
                if (config_.connection_file.empty()) {
                    return error_response(
                        404, {"route_not_found", "sidecar shutdown is not available in this deployment mode"}, id);
                }
                if (shutdown_requested_.exchange(true)) {
                    return error_response(409, {"shutdown_in_progress", "server shutdown is already in progress"}, id);
                }
                return json_response(202, {{"data", {{"accepted", true}}}}, id);
            });
        app_.route_dynamic("/api/v1/roots")([this](const crow::request &request) { return roots_response(request); });
        app_.route_dynamic("/api/v1/workspaces")
            .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post)([this](const crow::request &request) {
                return request.method == crow::HTTPMethod::Get ? workspace_snapshot_response(request)
                                                               : workspace_create_response(request);
            });
        app_.route_dynamic("/api/v1/workspaces/recovery/reset")
            .methods(crow::HTTPMethod::Post)(
                [this](const crow::request &request) { return workspace_reset_response(request); });
        app_.route_dynamic("/api/v1/workspaces/<string>")
            .methods(crow::HTTPMethod::Patch,
                     crow::HTTPMethod::Delete)([this](const crow::request &request, std::string workspace_id) {
                return workspace_item_response(request, workspace_id);
            });
        app_.route_dynamic("/api/v1/host-directories/roots")(
            [this](const crow::request &request) { return host_directory_roots_response(request); });
        app_.route_dynamic("/api/v1/host-directories/list")
            .methods(crow::HTTPMethod::Post)(
                [this](const crow::request &request) { return host_directory_listing_response(request); });
        app_.route_dynamic("/api/v1/files/list").methods(crow::HTTPMethod::Post)([this](const crow::request &request) {
            return directory_listing_response(request);
        });
        app_.route_dynamic("/api/v1/files/metadata")
            .methods(crow::HTTPMethod::Post)(
                [this](const crow::request &request) { return metadata_response(request); });
        app_.route_dynamic("/api/v1/files/content")(
            [this](const crow::request &request) { return download_response(request); });
        app_.route_dynamic("/api/v1/files/archive")
            .methods(crow::HTTPMethod::Post)(
                [this](const crow::request &request) { return create_download_archive_response(request); });
        app_.route_dynamic("/api/v1/download-archives/<string>/content")
            .methods(crow::HTTPMethod::Get,
                     crow::HTTPMethod::Delete)([this](const crow::request &request, std::string archive_id) {
                return download_archive_response(request, archive_id);
            });
        app_.route_dynamic("/api/v1/images").methods(crow::HTTPMethod::Post)([this](const crow::request &request) {
            return create_image_response(request);
        });
        app_.route_dynamic("/api/v1/images/<string>")
            .methods(crow::HTTPMethod::Get,
                     crow::HTTPMethod::Delete)([this](const crow::request &request, std::string image_id) {
                return image_response(request, image_id);
            });
        app_.route_dynamic("/api/v1/images/<string>/content")(
            [this](const crow::request &request, std::string image_id) {
                return image_content_response(request, image_id);
            });
        app_.route_dynamic("/api/v1/images/<string>/objects")(
            [this](const crow::request &request, std::string image_id) {
                return image_objects_response(request, image_id);
            });
        app_.route_dynamic("/api/v1/images/<string>/relationships")(
            [this](const crow::request &request, std::string image_id) {
                return image_relationships_response(request, image_id);
            });
        app_.route_dynamic("/api/v1/images/<string>/validation/issues")(
            [this](const crow::request &request, std::string image_id) {
                return image_validation_response(request, image_id);
            });
        app_.route_dynamic("/api/v1/images/<string>/preview")(
            [this](const crow::request &request, std::string image_id) {
                return image_preview_response(request, image_id);
            });
        app_.route_dynamic("/api/v1/uploads")
            .methods(crow::HTTPMethod::Post, crow::HTTPMethod::Options)([this](const crow::request &request) {
                if (request.method == crow::HTTPMethod::Options)
                    return preflight_response(request);
                return create_upload_response(request);
            });
        app_.route_dynamic("/api/v1/uploads/<string>")
            .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Put,
                     crow::HTTPMethod::Delete)([this](const crow::request &request, std::string upload_id) {
                return upload_response(request, upload_id);
            });
        app_.route_dynamic("/api/v1/uploads/<string>/complete")
            .methods(crow::HTTPMethod::Post)([this](const crow::request &request, std::string upload_id) {
                return complete_upload_response(request, upload_id);
            });
        app_.route_dynamic("/api/v1/uploads/<string>/materialize")
            .methods(crow::HTTPMethod::Post)([this](const crow::request &request, std::string upload_id) {
                return materialize_upload_response(request, upload_id);
            });
        app_.route_dynamic("/api/v1/jobs/<string>")
            .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Delete)(
                [this](const crow::request &request, std::string job_id) { return job_response(request, job_id); });
        app_.route_dynamic("/api/v1/jobs/<string>/events")(
            [this](const crow::request &request, std::string job_id) { return job_events_response(request, job_id); });
        app_.route_dynamic("/api/v1/event-tickets")
            .methods(crow::HTTPMethod::Post)(
                [this](const crow::request &request) { return event_ticket_response(request); });
        app_.route_dynamic("/api/v1/openapi.json")([this](const crow::request &request) {
            const auto id = request_id(request);
            if (auto denied = guard(request, id))
                return std::move(*denied);
            crow::response response{200, "application/json", openapi_document().dump()};
            response.set_header("X-Request-Id", id);
            return response;
        });
    }

    void register_event_route() {
        CROW_WEBSOCKET_ROUTE(app_, "/api/v1/events")
            .subprotocols({std::string{event_subprotocol}})
            .max_payload(config_.maximum_websocket_payload_bytes)
            .onaccept(std::function<void(const crow::request &, std::optional<crow::response> &, void **)>{
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
                }})
            .onopen([this](crow::websocket::connection &connection) {
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
            })
            .onmessage([](crow::websocket::connection &connection, const std::string &, bool) {
                connection.close("client messages are not accepted", crow::websocket::PolicyViolated);
            })
            .onclose([this](crow::websocket::connection &connection, const std::string &, std::uint16_t) {
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
            });
    }

    void register_operation_routes() {
        std::map<RouteKey, std::vector<std::string>> routes;
        for (const auto &entry : registry_.entries())
            routes[{entry.descriptor.method, entry.descriptor.route}].push_back(entry.descriptor.id);

        for (auto &[key, operation_ids] : routes) {
            auto &route = app_.route_dynamic(key.route);
            route.methods(key.method == axk::app::HttpMethod::get ? crow::HTTPMethod::Get : crow::HTTPMethod::Post);
            route([this, operation_ids = std::move(operation_ids)](const crow::request &request) {
                return operation_response(request, operation_ids);
            });
        }
    }

    axk::server::Config config_;
    axk::server::WorkspaceStore workspaces_;
    axk::app::Sandbox sandbox_;
    axk::app::UploadStore uploads_;
    axk::app::DownloadArchiveStore download_archives_;
    axk::app::OperationRegistry registry_;
    Json openapi_document_;
    axk::server::OpenApiValidator openapi_validator_;
    axk::app::ImageSessionManager images_;
    axk::app::JobManager jobs_;
    axk::server::EventTicketStore event_tickets_;
    axk::server::EventDispatcher event_dispatcher_;
    axk::server::RequestTelemetry request_telemetry_;
    std::mutex event_clients_mutex_;
    std::vector<EventClientHandle> event_clients_;
    axk::app::JobManager::SubscriptionId job_subscription_{};
    crow::App<CorsMiddleware, RequestTelemetryMiddleware> app_;
    std::atomic_bool shutdown_requested_{};
    std::atomic<std::uint64_t> websocket_clients_evicted_{};
    bool state_storage_ready_{};
    bool startup_cleanup_ready_{};
};

} // namespace

axk::app::Result<int> axk::server::run(const Config &config, app::OperationRegistry registry) {
    if (const auto valid = validate_config(config); !valid)
        return std::unexpected(valid.error());
    auto workspaces = WorkspaceStore::open(config.workspace_store);
    if (!workspaces)
        return std::unexpected(workspaces.error());
    ServerApplication application{config, std::move(registry), std::move(*workspaces)};
    return application.run();
}
