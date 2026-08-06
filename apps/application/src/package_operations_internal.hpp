#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/application/image_sessions.hpp"
#include "axklib/application/package_operations.hpp"
#include "axklib/media.hpp"
#include "axklib/package.hpp"
#include "package_filename.hpp"
#include "package_plan_store.hpp"

namespace axk::app::package_operations_internal {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using PackageInput = package_plan_internal::PackageInput;
using PackagePlanRecord = package_plan_internal::Record;
using PackagePlanStore = package_plan_internal::Store;

struct ResolvedPackage {
    std::shared_ptr<const axk::RandomAccessReader> reader;
    std::string filename;
    std::optional<UploadLease> lease;
};

struct VerifiedPackageSnapshot {
    PackageInput input;
    axk::PortablePackage package;
    std::uint64_t retained_payload_bytes{};
};

struct SessionPackagePlanRecord {
    std::string token;
    std::string owner_id;
    Clock::time_point expires_at;
    std::string image_id;
    std::uint64_t expected_revision{};
    std::shared_ptr<const VerifiedPackageSnapshot> package_snapshot;
    axk::PackageImportPlan plan;
    bool claimed{};
};

struct SessionPackageOperationState {
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<SessionPackagePlanRecord>> plans;
    std::chrono::minutes retention{15};
    std::size_t maximum_plans{128U};
    std::uint64_t maximum_retained_package_bytes{512U * 1024U * 1024U};
};

class TemporaryDirectoryCleanup {
  public:
    explicit TemporaryDirectoryCleanup(std::filesystem::path path) : path_{std::move(path)} {}
    ~TemporaryDirectoryCleanup() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

  private:
    std::filesystem::path path_;
};

class SessionPackagePlanClaim {
  public:
    SessionPackagePlanClaim(std::shared_ptr<SessionPackageOperationState> state,
                            std::shared_ptr<SessionPackagePlanRecord> record)
        : state_(std::move(state)), record_(std::move(record)) {}
    ~SessionPackagePlanClaim() { release(); }
    SessionPackagePlanClaim(const SessionPackagePlanClaim &) = delete;
    SessionPackagePlanClaim &operator=(const SessionPackagePlanClaim &) = delete;
    SessionPackagePlanClaim(SessionPackagePlanClaim &&other) noexcept
        : state_(std::move(other.state_)), record_(std::move(other.record_)),
          active_(std::exchange(other.active_, false)) {}
    SessionPackagePlanClaim &operator=(SessionPackagePlanClaim &&) = delete;

    [[nodiscard]] const std::shared_ptr<SessionPackagePlanRecord> &record() const noexcept { return record_; }

    void consume() {
        if (!active_)
            return;
        std::lock_guard lock{state_->mutex};
        state_->plans.erase(record_->token);
        active_ = false;
    }

  private:
    void release() {
        if (!active_)
            return;
        std::lock_guard lock{state_->mutex};
        if (const auto found = state_->plans.find(record_->token); found != state_->plans.end())
            found->second->claimed = false;
        active_ = false;
    }

    std::shared_ptr<SessionPackageOperationState> state_;
    std::shared_ptr<SessionPackagePlanRecord> record_;
    bool active_{true};
};

class SessionMutationGuard {
  public:
    SessionMutationGuard(ImageSessionManager &images, std::string_view image_id, std::string_view owner_id,
                         std::uint64_t revision)
        : images_(images), image_id_(image_id), owner_id_(owner_id), revision_(revision) {}
    ~SessionMutationGuard() {
        if (active_)
            images_.abort_mutation(image_id_, owner_id_, revision_);
    }
    void finish() noexcept { active_ = false; }

  private:
    ImageSessionManager &images_;
    std::string image_id_;
    std::string owner_id_;
    std::uint64_t revision_{};
    bool active_{true};
};

std::string normalized_path(const std::filesystem::path &path);
Error operation_error(std::string code, std::string message, std::optional<std::string> relative_path = std::nullopt,
                      bool retryable = false);
Error core_error(const axk::Error &error, std::optional<std::string> relative_path = std::nullopt);
Result<void> write_reader(const std::filesystem::path &path, const axk::RandomAccessReader &reader);
Result<FileRef> parse_file_ref(const Json &input, std::string_view field);
Json file_ref_json(const FileRef &reference);
Result<PackageInput> parse_package_input(const Json &input);
Result<ResolvedPackage> resolve_package(const PackageInput &input, std::string_view owner_id, const Sandbox &sandbox,
                                        UploadStore &uploads);
Result<axk::PortablePackage> read_package(const ResolvedPackage &resolved, bool verify,
                                          const OperationContext &context);
Json package_json(const axk::PortablePackage &package);
Result<axk::PackageRootKind> parse_root_kind(std::string_view value);
Result<std::vector<axk::PackageRootSelector>> parse_roots(const Json &input);
Result<std::vector<axk::PackageRootSelector>>
parse_session_export_roots(const Json &input, const std::unordered_map<std::string, std::string> &object_keys_by_id,
                           const std::unordered_map<std::string, ImageVolumeScopeIdentity> &volume_scopes_by_id);
Result<axk::PackageImportRequest> parse_import_request(const Json &input);
std::string target_kind_name(axk::MediaKind kind);
Json program_assignment_adjustments_json(std::span<const axk::PackageProgramAssignmentAdjustment> adjustments);
Json plan_json(const axk::PackageImportPlan &plan, std::string_view token, std::uint64_t expires_in_seconds);
void cleanup_session_plans(SessionPackageOperationState &state, Clock::time_point now);
Result<std::uint64_t> retained_package_bytes(const axk::PortablePackage &package);
std::uint64_t retained_session_package_bytes(const SessionPackageOperationState &state);
Result<SessionPackagePlanClaim> claim_session_plan(const std::shared_ptr<SessionPackageOperationState> &state,
                                                   std::string_view token, std::string_view owner_id);
Result<std::pair<std::string, std::uint64_t>> parse_session_identity(const Json &input);
Result<axk::PackageImportRequest> parse_session_import_request(const Json &input, const axk::PortablePackage &package);
Json session_import_result(const SessionPackagePlanRecord &record, const ImageSessionSummary &summary, bool applied);
Result<Json> read_operation(const Json &input, const OperationContext &context, const Sandbox &sandbox,
                            UploadStore &uploads, bool verify);

} // namespace axk::app::package_operations_internal
