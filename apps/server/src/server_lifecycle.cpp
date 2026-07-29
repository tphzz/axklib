#include "server_application.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <future>
#include <iostream>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

#include "authentication.hpp"
#include "axklib/application/application_operations.hpp"
#include "axklib/server/process_lifetime.hpp"
#include "axklib/server/server.hpp"
#include "axklib/version.hpp"
#include "server_support.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <aclapi.h>
#include <process.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace axk::server::detail {
namespace {
std::uint64_t process_id() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

#ifdef _WIN32
axk::app::Result<void> write_owner_only_file(const std::filesystem::path &path, std::string_view content) {
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
}
#endif
class ScopedConnectionFile {
  public:
    explicit ScopedConnectionFile(std::filesystem::path path) : path_(std::move(path)) {}

    ScopedConnectionFile(const ScopedConnectionFile &) = delete;
    ScopedConnectionFile &operator=(const ScopedConnectionFile &) = delete;

    ~ScopedConnectionFile() {
        if (published_) {
#ifdef _WIN32
            std::error_code error;
            std::filesystem::remove(path_, error);
#else
            if (parent_descriptor_ >= 0) {
                static_cast<void>(::unlinkat(parent_descriptor_, filename_.c_str(), 0));
                static_cast<void>(::fsync(parent_descriptor_));
            }
#endif
        }
#ifndef _WIN32
        if (parent_descriptor_ >= 0)
            static_cast<void>(::close(parent_descriptor_));
#endif
    }

    axk::app::Result<void> publish(const Json &document) {
#ifdef _WIN32
        std::error_code error;
        const auto parent = path_.parent_path();
        const auto metadata = std::filesystem::symlink_status(parent, error);
        if (error || !std::filesystem::is_directory(metadata) ||
            (GetFileAttributesW(parent.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            return std::unexpected(axk::app::Error{"connection_file_failed",
                                                   "connection-file parent must be an existing private directory"});
        }
        if (std::filesystem::exists(path_, error) || error)
            return std::unexpected(axk::app::Error{"connection_file_failed", "connection file already exists"});
        auto temporary = path_;
        temporary += ".tmp." + std::to_string(process_id());
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
#else
        const auto parent = path_.parent_path();
        filename_ = path_.filename().string();
        if (parent.empty() || filename_.empty() || filename_ == "." || filename_ == "..") {
            return std::unexpected(axk::app::Error{"connection_file_failed",
                                                   "connection-file parent must be an existing private directory"});
        }
        parent_descriptor_ = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (parent_descriptor_ < 0)
            return std::unexpected(
                axk::app::Error{"connection_file_failed", "could not retain the connection-file directory"});
        struct stat parent_status{};
        if (::fstat(parent_descriptor_, &parent_status) != 0 || !S_ISDIR(parent_status.st_mode) ||
            parent_status.st_uid != ::geteuid() || (parent_status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
            return std::unexpected(
                axk::app::Error{"connection_file_failed", "connection-file parent is not an owner-only directory"});
        }
        struct stat existing{};
        if (::fstatat(parent_descriptor_, filename_.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
            return std::unexpected(axk::app::Error{"connection_file_failed", "connection file already exists"});
        }
        temporary_name_ = std::format(".{}.tmp.{}", filename_, process_id());
        const auto temporary =
            ::openat(parent_descriptor_, temporary_name_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (temporary < 0)
            return std::unexpected(
                axk::app::Error{"connection_file_failed", "could not create the owner-only connection file"});
        const auto contents = document.dump(2) + '\n';
        std::size_t offset{};
        while (offset < contents.size()) {
            const auto written = ::write(temporary, contents.data() + offset, contents.size() - offset);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0) {
                static_cast<void>(::close(temporary));
                static_cast<void>(::unlinkat(parent_descriptor_, temporary_name_.c_str(), 0));
                return std::unexpected(
                    axk::app::Error{"connection_file_failed", "could not write the connection file"});
            }
            offset += static_cast<std::size_t>(written);
        }
        const auto flush_failed = ::fsync(temporary) != 0;
        const auto close_failed = ::close(temporary) != 0;
        if (flush_failed || close_failed) {
            static_cast<void>(::unlinkat(parent_descriptor_, temporary_name_.c_str(), 0));
            return std::unexpected(axk::app::Error{"connection_file_failed", "could not flush the connection file"});
        }
        if (::renameat(parent_descriptor_, temporary_name_.c_str(), parent_descriptor_, filename_.c_str()) != 0 ||
            ::fsync(parent_descriptor_) != 0) {
            static_cast<void>(::unlinkat(parent_descriptor_, temporary_name_.c_str(), 0));
            static_cast<void>(::unlinkat(parent_descriptor_, filename_.c_str(), 0));
            return std::unexpected(axk::app::Error{"connection_file_failed", "could not publish the connection file"});
        }
        published_ = true;
        return {};
#endif
    }

  private:
    std::filesystem::path path_;
#ifndef _WIN32
    int parent_descriptor_{-1};
    std::string filename_;
    std::string temporary_name_;
#endif
    bool published_{};
};

} // namespace

ServerApplication::ServerApplication(axk::server::Config config, axk::app::OperationRegistry registry,
                                     axk::server::WorkspaceStore workspaces)
    : config_(std::move(config)), workspaces_(std::move(workspaces)), sandbox_(workspaces_.sandbox()),
      uploads_(upload_directory(), config_.maximum_upload_total_bytes, config_.maximum_upload_bytes,
               config_.maximum_uploads, config_.maximum_upload_chunk_bytes,
               std::chrono::seconds{config_.upload_retention_seconds}),
      download_archives_(download_archive_directory(),
                         {config_.maximum_download_archive_total_bytes, config_.maximum_download_archive_bytes,
                          config_.maximum_download_archive_entries, config_.maximum_download_archive_depth,
                          config_.maximum_download_archive_path_bytes},
                         std::chrono::seconds{config_.download_archive_retention_seconds}),
      archive_download_budget_(config_.maximum_concurrent_archive_downloads),
      alteration_journals_(alteration_journal_directory()),
      images_(sandbox_, config_.maximum_image_sessions, config_.maximum_page_size,
              std::chrono::seconds{config_.image_idle_seconds}, std::chrono::steady_clock::now, &path_reservations_,
              config_.maximum_audition_bundle_bytes),
      registry_(prepare_registry(std::move(registry), sandbox_, uploads_, images_, alteration_journals_,
                                 download_archives_,
                                 {config_.maximum_media_build_object_bytes, config_.maximum_media_build_payload_bytes,
                                  config_.maximum_media_build_output_bytes})),
      openapi_document_(axk::server::build_openapi_document(axk::server::embedded_openapi(), registry_)),
      openapi_validator_(openapi_document_),
      jobs_(
          registry_, config_.job_worker_threads, config_.write_job_worker_threads, config_.maximum_queued_jobs,
          config_.replay_events_per_job, config_.maximum_retained_jobs,
          std::chrono::seconds{config_.job_retention_seconds}, [] { return axk::app::JobManager::Clock::now(); },
          &uploads_, &path_reservations_),
      event_tickets_(std::chrono::seconds{config_.event_ticket_ttl_seconds}, config_.maximum_event_tickets),
      event_dispatcher_(config_.maximum_websocket_delivery_events,
                        [this](const axk::app::JobEvent &event) { broadcast(event); }) {
    app_.template get_middleware<CorsMiddleware>().allowed_origins = config_.allowed_origins;
    app_.template get_middleware<RequestTelemetryMiddleware>().telemetry = &request_telemetry_;
    const auto journal_recovery = alteration_journals_.recover(sandbox_);
    state_storage_ready_ = uploads_.storage_ready() && download_archives_.storage_ready() &&
                           alteration_journals_.storage_ready() && journal_recovery &&
                           writable_directory(upload_directory()) && writable_directory(download_archive_directory()) &&
                           writable_directory(alteration_journal_directory());
    const auto publication_cleanup = sandbox_.cleanup_abandoned_publications();
    startup_cleanup_ready_ = publication_cleanup && state_storage_ready_;
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

ServerApplication::~ServerApplication() {
    jobs_.unsubscribe(job_subscription_);
    jobs_.shutdown();
    event_dispatcher_.shutdown();
}

axk::app::Result<int> ServerApplication::run() {
    std::future<void> server;
    const auto stop_server = [&]() noexcept {
        try {
            app_.stop();
        } catch (...) {
        }
        if (server.valid()) {
            try {
                server.wait();
            } catch (...) {
            }
        }
    };
    try {
        app_.bindaddr(config_.bind_address)
            .port(config_.port)
            .concurrency(config_.worker_threads)
            .stream_threshold(config_.stream_threshold_bytes)
            .max_request_body(std::max(config_.maximum_json_bytes, config_.maximum_upload_chunk_bytes))
            .websocket_max_payload(config_.maximum_websocket_payload_bytes)
            .server_name("axklib-server");

        server = app_.run_async();
        const auto start_status = app_.wait_for_server_start(std::chrono::seconds{5});
        if (start_status == std::cv_status::timeout || !app_.is_bound()) {
            stop_server();
            return std::unexpected(
                axk::app::Error{"server_start_failed", "Crow could not bind the configured endpoint"});
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
                stop_server();
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
    } catch (const std::exception &error) {
        stop_server();
        return std::unexpected(
            axk::app::Error{"server_start_failed", "server transport failed: " + std::string{error.what()}});
    } catch (...) {
        stop_server();
        return std::unexpected(axk::app::Error{"server_start_failed", "server transport failed"});
    }
}

axk::app::OperationRegistry ServerApplication::prepare_registry(
    axk::app::OperationRegistry registry, const axk::app::Sandbox &sandbox, axk::app::UploadStore &uploads,
    axk::app::ImageSessionManager &images, axk::app::AlterationJournalStore &journals,
    axk::app::DownloadArchiveStore &downloads, const axk::MediaBuildLimits &media_limits) {
    auto prepared = axk::app::make_application_registry(sandbox, uploads, std::move(registry), media_limits);
    if (!prepared)
        std::terminate();
    if (const auto session_operations =
            axk::app::bind_session_application_operations(*prepared, sandbox, uploads, images, journals, downloads);
        !session_operations) {
        std::terminate();
    }
    return std::move(*prepared);
}

[[nodiscard]] std::filesystem::path ServerApplication::upload_directory() const {
    if (!config_.state_directory.empty())
        return config_.state_directory / "uploads";
    return std::filesystem::temp_directory_path() / "axklib-server" / "uploads";
}

[[nodiscard]] std::filesystem::path ServerApplication::download_archive_directory() const {
    if (!config_.state_directory.empty())
        return config_.state_directory / "download-archives";
    return std::filesystem::temp_directory_path() / "axklib-server" / "download-archives";
}

[[nodiscard]] std::filesystem::path ServerApplication::alteration_journal_directory() const {
    if (!config_.state_directory.empty())
        return config_.state_directory / "alteration-journals";
    return std::filesystem::temp_directory_path() / "axklib-server" / "alteration-journals";
}

} // namespace axk::server::detail
