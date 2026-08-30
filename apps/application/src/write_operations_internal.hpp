#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/alteration.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/write_operations.hpp"
#include "axklib/media.hpp"

namespace axk::app::write_operations_internal {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

class TemporaryDirectoryCleanup;

struct ManifestDocument {
    Json json;
    std::vector<UploadLease> leases;
    std::vector<std::filesystem::path> observed_paths;
    std::vector<std::filesystem::path> bound_input_paths;
    std::vector<std::filesystem::path> upload_input_paths;
    std::vector<FileRef> file_inputs;
    std::vector<std::string> file_input_sha256;
    std::map<std::string, std::string> logical_input_paths;
    std::vector<std::shared_ptr<TemporaryDirectoryCleanup>> staging;
};

enum class WritePlanKind : std::uint8_t { hds, floppy, iso };

struct FileFingerprint {
    std::filesystem::path path;
    std::string sha256;
    std::uintmax_t size{};
    std::filesystem::file_time_type last_write_time;
};

struct WritePlanRecord {
    std::string token;
    std::string owner_id;
    Clock::time_point expires_at;
    WritePlanKind kind{WritePlanKind::hds};
    FileRef output;
    std::filesystem::path output_path;
    bool overwrite{};
    bool output_existed{};
    std::optional<std::string> output_sha256;
    std::vector<FileFingerprint> inputs;
    std::vector<FileRef> input_refs;
    std::vector<std::string> input_ref_sha256;
    std::map<std::string, std::string> logical_input_paths;
    std::vector<UploadLease> leases;
    std::vector<std::shared_ptr<TemporaryDirectoryCleanup>> staging;
    std::variant<axk::HdsBuildManifest, axk::MediaBuildManifest, axk::FloppyCreationPlan> manifest;
    Json summary;
    std::string semantic_version;
    std::string source_identity;
    bool claimed{};
};

struct WriteOperationState {
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<WritePlanRecord>> plans;
    std::unordered_map<std::string, std::string> destination_reservations;
    std::chrono::minutes retention{15};
    std::size_t maximum_plans{128U};
};

class TemporaryDirectoryCleanup {
  public:
    explicit TemporaryDirectoryCleanup(std::filesystem::path path);
    ~TemporaryDirectoryCleanup();
    TemporaryDirectoryCleanup(const TemporaryDirectoryCleanup &) = delete;
    TemporaryDirectoryCleanup &operator=(const TemporaryDirectoryCleanup &) = delete;

  private:
    std::filesystem::path path_;
};

struct ResolvedInput {
    std::filesystem::path path;
    std::optional<UploadLease> lease;
    std::shared_ptr<TemporaryDirectoryCleanup> staging;
};

class WritePlanClaim {
  public:
    WritePlanClaim(std::shared_ptr<WriteOperationState> state, std::string token,
                   std::shared_ptr<WritePlanRecord> record);
    WritePlanClaim(const WritePlanClaim &) = delete;
    WritePlanClaim &operator=(const WritePlanClaim &) = delete;
    WritePlanClaim(WritePlanClaim &&other) noexcept;
    WritePlanClaim &operator=(WritePlanClaim &&) = delete;
    ~WritePlanClaim();

    void consume();
    [[nodiscard]] const std::shared_ptr<WritePlanRecord> &record() const noexcept;

  private:
    void release();

    std::shared_ptr<WriteOperationState> state_;
    std::string token_;
    std::shared_ptr<WritePlanRecord> record_;
    bool active_{true};
};

Error operation_error(std::string code, std::string message, std::optional<std::string> relative_path = std::nullopt);
Error core_error(const axk::Error &error, std::optional<std::string> relative_path = std::nullopt);
std::string write_plan_kind_name(WritePlanKind kind);
WritePlanKind write_plan_kind(axk::BuildManifestKind kind);
std::string_view hds_creation_profile_wire_id(axk::HdsCreationProfileId id);
std::optional<axk::HdsCreationProfileId> parse_hds_creation_profile_wire_id(std::string_view id);
std::string normalized_path(const std::filesystem::path &path);
void cleanup_plans(WriteOperationState &state, Clock::time_point now);
Result<WritePlanClaim> claim_plan(const std::shared_ptr<WriteOperationState> &state, std::string_view token,
                                  std::string_view owner_id, WritePlanKind expected_kind);
Result<FileRef> parse_file_ref(const Json &input, std::string_view field);
Json file_ref_json(const FileRef &reference);
Result<void> write_reader(const std::filesystem::path &path, const axk::RandomAccessReader &reader);
Result<ManifestDocument> load_manifest(const Json &input, const OperationContext &context, const Sandbox &sandbox,
                                       UploadStore &uploads);
std::vector<std::filesystem::path> external_paths(const axk::HdsBuildManifest &manifest);
std::vector<std::filesystem::path> external_paths(const axk::MediaBuildManifest &manifest);
std::vector<std::filesystem::path> external_paths(const axk::AlterationManifest &manifest);
Result<void> require_bound_inputs(std::span<const std::filesystem::path> required,
                                  std::span<const std::filesystem::path> admitted);
bool contains_path(std::span<const std::filesystem::path> paths, const std::filesystem::path &candidate);
Result<std::vector<FileFingerprint>> fingerprint_files(std::span<const std::filesystem::path> paths,
                                                       const CancellationToken &cancellation);
Result<void> verify_fingerprints(std::span<const FileFingerprint> fingerprints, const CancellationToken &cancellation);
std::optional<std::string> known_fingerprint(std::span<const FileFingerprint> fingerprints,
                                             const std::filesystem::path &path);
Result<void> verify_plan_state(const WritePlanRecord &record, const Sandbox &sandbox,
                               const CancellationToken &cancellation);
Result<void> verify_sandbox_files(std::span<const FileRef> references, std::span<const std::string> expected_sha256,
                                  const Sandbox &sandbox, const CancellationToken &cancellation);
Result<void> verify_alteration_state(std::span<const FileFingerprint> inputs, const std::filesystem::path &output_path,
                                     bool output_existed, const std::optional<std::string> &output_sha256,
                                     const CancellationToken &cancellation);
Result<void> register_plan(const std::shared_ptr<WriteOperationState> &state,
                           const std::shared_ptr<WritePlanRecord> &record);
Json write_plan_json(const WritePlanRecord &record, std::uint64_t expires_in_seconds);
Json manifest_choices(axk::BuildManifestKind kind);
Json alteration_manifest_choices();

void decorate_build_result(Json &result, const WritePlanRecord &record);
Result<Json> validate_written_image(const std::filesystem::path &path, const FileRef &output,
                                    const OperationContext &context);
Result<axk::BuildManifestKind> parse_build_kind(std::string_view value);
Json operation_report_json(const axk::OperationReport &operation,
                           const std::map<std::string, std::string> &logical_input_paths);
Json alteration_summary(std::span<const axk::OperationReport> operations);
Json deletion_inspection_json(const ImageObjectDeletionInspection &inspection);
Json wave_data_orphan_inspection_json(const ImageWaveDataOrphanInspection &inspection);
Json program_generation_inspection_json(const ImageProgramGenerationInspection &inspection);
Json program_assignment_cleanup_inspection_json(const ImageProgramAssignmentCleanupInspection &inspection);
Json deletion_manifest_json(const axk::AlterationManifest &manifest);
Json program_generation_manifest_json(const axk::AlterationManifest &manifest);
Json program_assignment_cleanup_manifest_json(const axk::AlterationManifest &manifest);
axk::Result<PreparedLocalBuildManifest> prepare_local_manifest_document(const std::filesystem::path &manifest_path,
                                                                        std::span<const std::filesystem::path> paths);

} // namespace axk::app::write_operations_internal
