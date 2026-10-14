#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/application/allocation_map.hpp"
#include "axklib/application/contracts.hpp"
#include "axklib/application/device_sessions.hpp"
#include "axklib/application/filesystem.hpp"
#include "axklib/application/image_documents.hpp"
#include "axklib/application/image_filesystem.hpp"
#include "axklib/application/path_reservations.hpp"
#include "axklib/deletion.hpp"
#include "axklib/export.hpp"
#include "axklib/filesystem_import.hpp"
#include "axklib/io.hpp"
#include "axklib/media.hpp"
#include "axklib/types.hpp"
#include "axklib/volume_capacity.hpp"

namespace axk::app {

namespace detail {
struct ImageFilesystemIndex;
}

namespace formats::a_series {
class SessionReader;
}

struct ImageSessionSummary {
    std::string image_id;
    std::uint64_t revision{};
    std::vector<ImageSourceSummary> sources;
    std::vector<ImageDocumentSummary> documents;
    ImageDocumentDiscovery discovery;
};

struct ImageDocumentContext {
    std::string image_id;
    std::string document_id;
    std::uint64_t revision{};
    std::string format;
    std::vector<std::string> available_operations;
};

enum class CompanionSelectionKind : std::uint8_t { sources, immediate_siblings };

struct CompanionSelection {
    CompanionSelectionKind kind{CompanionSelectionKind::sources};
    std::vector<ImageSourceRef> sources;
};

struct ImageSessionMutation {
    std::string image_id;
    std::uint64_t revision{};
    ImageSourceRef source;
    std::shared_ptr<MutationTarget> target;
    MediaKind media_kind{MediaKind::sfs};
    std::string source_member_id;
};

struct PreparedImageSessionCommit {
    std::string image_id;
    std::uint64_t expected_revision{};
    ImageSessionSummary summary;

  private:
    std::shared_ptr<void> current_state;
    std::shared_ptr<void> refreshed_state;
    friend class ImageSessionManager;
};

struct ImageSessionRead {
    std::string image_id;
    std::uint64_t revision{};
    ImageSourceRef source;
    std::shared_ptr<const RandomAccessReader> reader;
    const MediaContainer *media{};
    // File SHA-256 or an explicit guarded device-session revision; never hashes a whole device.
    std::function<Result<std::string>(const CancellationToken &)> content_fingerprint;
    std::shared_ptr<void> lease;
    std::function<Result<void>()> verify_source_unchanged;
    std::string source_member_id;
    std::string document_id;
    std::shared_ptr<const detail::ImageFilesystemIndex> filesystem{};
};

struct ImagePartitionCapacity {
    std::uint32_t allocated_clusters{};
    std::uint32_t free_clusters{};
    std::uint32_t cluster_size_bytes{};
};

struct ImageContentItem {
    std::string id;
    std::optional<std::string> parent_id;
    std::size_t depth{};
    std::optional<std::uint8_t> partition_index;
    std::optional<std::uint32_t> volume_directory_id;
    std::optional<ImagePartitionCapacity> partition_capacity;
    std::optional<std::uint64_t> size_bytes;
    std::string kind;
    std::string name;
    std::string display_name;
    std::size_t child_count{};
    std::optional<std::string> object_id;
    std::optional<std::string> object_type;
    std::string scope_role;
    std::string quality;
    std::string basis;
    std::string notes;
    std::vector<std::string> details;
};

struct ImageContentScope {
    ImageContentItem item;
    std::vector<ImageContentItem> children;
};

struct WaveformMetadata {
    std::uint16_t sample_rate{};
    std::uint16_t sample_width_bytes{};
    std::string embedded_container_name;
    std::uint8_t root_key{};
    std::int8_t fine_tune_cents{};
    std::uint8_t loop_mode{};
    std::string loop_mode_label;
    std::uint32_t stored_frame_count{};
    std::uint32_t wave_start_frame{};
    std::uint32_t wave_length_frames{};
    std::uint32_t loop_start_frame{};
    std::uint32_t loop_length_frames{};
    std::string storage_state;
};

struct SequenceMetadata {
    struct TempoEvent {
        std::uint32_t tick{};
        std::uint32_t microseconds_per_quarter_note{};
    };

    std::uint16_t format_version{};
    std::uint16_t ticks_per_quarter_note{};
    std::uint32_t first_tick{};
    std::uint32_t end_tick{};
    std::uint64_t event_count{};
    std::optional<std::uint16_t> header_tempo_bpm;
    std::uint32_t effective_initial_tempo_microseconds_per_quarter_note{};
    std::vector<TempoEvent> tempo_events;
};

struct ImageObjectItem {
    std::string id;
    std::string type;
    std::string name;
    std::string format;
    std::optional<std::uint8_t> partition_index;
    std::string partition_name;
    std::string volume_name;
    std::string category_name;
    std::string entry_name;
    std::uint64_t stored_size_bytes{};
    std::optional<std::uint64_t> size_with_dependencies_bytes;
    std::optional<WaveformMetadata> waveform;
    std::optional<SequenceMetadata> sequence;
    nlohmann::json sample_format = nullptr;
    nlohmann::json program_format = nullptr;
};

struct ImageRelationshipItem {
    std::string id;
    std::string source_object_id;
    std::optional<std::string> target_object_id;
    std::vector<std::string> candidate_object_ids;
    std::string type;
    std::string quality;
    std::string basis;
    std::string notes;
    std::optional<std::size_t> assignment_index;
    std::string assignment_name;
    std::string assignment_state;
    std::string receive_channel_display;
};

struct ImageRelationshipFilter {
    std::optional<std::string_view> content_scope_id;
    std::optional<std::string_view> source_object_id;
    std::optional<std::string_view> target_object_id;
    std::optional<std::string_view> relationship_type;
};

enum class SystemProgramContextAvailability : std::uint8_t { available, not_present, invalid };

enum class SystemProgramContextFile : std::uint8_t { system, system2 };

struct ImageSystemMidiAddress {
    std::string port;
    std::uint8_t channel{1U};
    std::string display;
};

struct ImageSystemProgramPart {
    std::uint8_t part_number{1U};
    std::string part_label;
    ImageSystemMidiAddress midi;
    std::uint16_t program_number{1U};
    bool master{};
};

struct ImageSystemProgramContext {
    SystemProgramContextFile file_kind{SystemProgramContextFile::system};
    SystemProgramContextAvailability availability{SystemProgramContextAvailability::not_present};
    std::uint8_t storage_revision{};
    std::optional<std::string> saved_program_mode;
    std::optional<ImageSystemMidiAddress> basic_receive;
    std::optional<bool> omni;
    std::optional<bool> program_change_enabled;
    std::vector<ImageSystemProgramPart> parts;
    std::string message;
};

struct ImageSystemProgramContexts {
    std::uint8_t partition_index{};
    std::vector<ImageSystemProgramContext> files;
    std::string message;
};

struct ImageObjectDeletionNotice {
    std::string code;
    std::string message;
    std::vector<std::string> object_ids;
};

struct ImageObjectDeletionImpact {
    std::string object_id;
    std::string object_type;
    std::string object_name;
    std::optional<std::uint8_t> partition_index;
    std::string partition_name;
    std::string volume_name;
    std::string role;
    std::string status;
    bool requested{};
    bool selected{};
    std::uint64_t stored_size_bytes{};
    std::uint64_t freed_clusters{};
    std::vector<std::string> prerequisite_object_ids;
    std::string reason;
};

struct ImageObjectDeletionReference {
    std::string source_object_id;
    std::string source_object_type;
    std::string source_object_name;
    std::optional<std::string> target_object_id;
    std::optional<std::string> target_object_type;
    std::optional<std::string> target_object_name;
    std::string type;
    std::string quality;
    std::string effect;
};

struct ImageObjectDeletionInspection {
    bool can_apply{};
    std::string image_id;
    std::uint64_t revision{};
    std::vector<std::string> target_object_ids;
    std::vector<std::string> referrer_object_ids;
    std::vector<std::string> cleanup_object_ids;
    std::vector<std::string> selected_object_ids;
    std::vector<ImageObjectDeletionImpact> impacts;
    std::vector<ImageObjectDeletionReference> references;
    std::vector<ImageObjectDeletionNotice> blockers;
    std::vector<ImageObjectDeletionNotice> warnings;
    std::uint64_t estimated_freed_bytes{};
    std::uint64_t estimated_freed_clusters{};
};

struct ImageObjectDeletionPlan {
    ImageObjectDeletionInspection inspection;
    axk::AlterationManifest manifest;
};

struct ImageWaveDataOrphanCandidate {
    std::string object_id;
    std::string object_type;
    std::string object_name;
    std::optional<std::uint8_t> partition_index;
    std::string partition_name;
    std::string volume_name;
    std::uint64_t stored_size_bytes{};
    std::uint64_t recoverable_bytes{};
    std::uint64_t recoverable_clusters{};
};

struct ImageWaveDataOrphanInspection {
    std::string image_id;
    std::uint64_t revision{};
    std::string content_scope_id;
    std::size_t total_candidate_count{};
    std::vector<ImageWaveDataOrphanCandidate> candidates;
};

struct ImageProgramGenerationNotice {
    std::string code;
    std::string message;
    std::vector<std::string> object_ids;
};

struct ImageProgramGenerationCandidate {
    std::string target_object_id;
    std::string target_object_type;
    std::string target_object_name;
    std::string default_program_name;
    std::optional<std::uint8_t> program_number;
    bool default_selected{};
};

struct ImageProgramGenerationInspection {
    std::string image_id;
    std::uint64_t revision{};
    std::string content_scope_id;
    std::vector<std::uint8_t> available_program_numbers;
    std::vector<ImageProgramGenerationCandidate> candidates;
    std::vector<ImageProgramGenerationNotice> notices;
};

struct ImageProgramGenerationSelection {
    std::string target_object_id;
    std::uint8_t program_number{};
    std::string program_name;
};

struct ImageProgramGenerationPlan {
    ImageProgramGenerationInspection inspection;
    std::vector<ImageProgramGenerationSelection> selections;
    axk::AlterationManifest manifest;
};

struct ImageProgramAssignmentCleanupCandidate {
    std::string program_object_id;
    std::uint8_t program_number{};
    std::string program_name;
    std::uint16_t assignment_ordinal{};
    std::string assignment_name;
    std::string target_object_type;
    std::string receive_channel_display;
    std::string reason;
    std::size_t candidate_target_count{};
    bool default_selected{true};
};

struct ImageProgramAssignmentCleanupInspection {
    std::string image_id;
    std::uint64_t revision{};
    std::string content_scope_id;
    std::size_t total_candidate_count{};
    std::vector<ImageProgramAssignmentCleanupCandidate> candidates;
};

struct ImageProgramAssignmentCleanupSelection {
    std::string program_object_id;
    std::uint16_t assignment_ordinal{};
};

struct ImageProgramAssignmentCleanupPlan {
    ImageProgramAssignmentCleanupInspection inspection;
    std::vector<ImageProgramAssignmentCleanupSelection> selections;
    axk::AlterationManifest manifest;
};

struct ImageValidationItem {
    std::string code;
    std::string severity;
    std::string message;
    std::string sampler_path;
    std::optional<std::string> object_id;
};

struct ImagePreviewBin {
    std::int32_t minimum{};
    std::int32_t maximum{};
};

struct ImageWaveformPreviewLane {
    std::string role;
    std::string source_object_id;
    std::uint32_t sample_rate{};
    std::uint16_t sample_width_bytes{};
    std::uint64_t stored_frame_count{};
    std::uint64_t playback_start_frame{};
    std::uint64_t playback_length_frames{};
    std::uint64_t loop_start_frame{};
    std::uint64_t loop_length_frames{};
    std::vector<ImagePreviewBin> bins;
};

struct ImageWaveformPreview {
    std::string object_id;
    std::vector<ImageWaveformPreviewLane> lanes;
};

struct ImageAuditionLane {
    std::string role;
    std::string source_object_id;
    std::uint32_t sample_rate{};
    std::uint16_t sample_width_bytes{};
    std::uint64_t frame_count{};
    std::uint64_t content_offset_bytes{};
    std::uint64_t wav_size_bytes{};
    std::uint64_t loop_start_frame{};
    std::uint64_t loop_length_frames{};
};

struct ImageAuditionClip {
    std::string object_id;
    std::uint8_t loop_mode{};
    std::string loop_mode_label;
    std::vector<std::string> warnings;
    std::vector<ImageAuditionLane> lanes;
};

struct ImageAudition {
    std::string audition_id;
    std::uint64_t content_size_bytes{};
    std::vector<ImageAuditionClip> clips;
};

struct ImageAuditionRange {
    std::uint64_t total_size{};
    std::vector<std::byte> bytes;
};

template <typename Item> struct ImagePage {
    std::vector<Item> items;
    std::size_t total_count{};
    std::optional<std::string> next_cursor;
};

class ImageSessionManager {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    ImageSessionManager(const Sandbox &sandbox, std::size_t maximum_sessions = 32U,
                        std::size_t maximum_page_size = 500U,
                        std::chrono::seconds idle_retention = std::chrono::minutes{15},
                        Clock clock = std::chrono::steady_clock::now,
                        PathReservationCoordinator *path_reservations = nullptr,
                        std::uint64_t maximum_audition_bundle_bytes = 128ULL * 1024ULL * 1024ULL,
                        std::size_t maximum_audition_clips = 256U, DeviceSessionManager *devices = nullptr);
    ~ImageSessionManager();
    ImageSessionManager(ImageSessionManager &&) noexcept;
    ImageSessionManager &operator=(ImageSessionManager &&) noexcept;
    ImageSessionManager(const ImageSessionManager &) = delete;
    ImageSessionManager &operator=(const ImageSessionManager &) = delete;

    [[nodiscard]] Result<ImageSessionSummary> open(const ImageOpenSources &sources, std::string owner_id,
                                                   const CancellationToken &cancellation = {},
                                                   ProgressSink *progress = nullptr,
                                                   diagnostic_sink *diagnostics = nullptr);
    [[nodiscard]] Result<ImageSessionSummary>
    attach_companions(const ImageDocumentRef &document, std::string_view owner_id, std::uint64_t expected_revision,
                      const CompanionSelection &selection, const CancellationToken &cancellation = {});
    [[nodiscard]] Result<ImageSessionSummary> inspect(std::string_view image_id, std::string_view owner_id);
    [[nodiscard]] Result<ImagePage<ImageDocumentSummary>>
    documents(std::string_view image_id, std::string_view owner_id, std::size_t limit,
              std::optional<std::string_view> cursor = std::nullopt);
    [[nodiscard]] Result<ImageDocumentSummary> document(const ImageDocumentRef &target, std::string_view owner_id);
    [[nodiscard]] Result<ImageSessionSummary> select_documents(std::string_view image_id, std::string_view owner_id,
                                                               std::uint64_t expected_revision,
                                                               const std::vector<std::string> &selected_formats,
                                                               const CancellationToken &cancellation = {});
    [[nodiscard]] Result<VolumeCapacityReport>
    volume_capacity(const ImageDocumentRef &target, std::string_view owner_id, std::uint64_t expected_revision,
                    std::string_view content_scope_id, const CancellationToken &cancellation = {});
    [[nodiscard]] Result<ImageFilesystemPage> filesystem(std::string_view image_id, std::string_view owner_id,
                                                         std::uint64_t expected_revision,
                                                         const ImageFilesystemQuery &query = {});
    [[nodiscard]] Result<ResolvedImageFilesystemEdits>
    inspect_su700_import(std::string_view image_id, std::string_view owner_id, std::uint64_t expected_revision,
                         std::string_view root_entry_id, std::string_view volume_name,
                         std::span<const FilesystemEdit> edits, const CancellationToken &cancellation = {});
    [[nodiscard]] Result<ResolvedImageFilesystemEdits>
    resolve_filesystem_edits(std::string_view image_id, std::string_view owner_id, std::uint64_t expected_revision,
                             std::span<const ImageFilesystemEdit> edits);
    [[nodiscard]] Result<std::vector<FilesystemImportDecision>>
    inspect_filesystem_import(std::string_view image_id, std::string_view owner_id, std::uint64_t expected_revision,
                              std::string_view parent_entry_id, std::span<const FilesystemImportEntry> entries,
                              const CancellationToken &cancellation = {});
    [[nodiscard]] Result<ImageObjectDeletionPlan> plan_deletion(const ImageDocumentRef &target,
                                                                std::string_view owner_id,
                                                                std::uint64_t expected_revision,
                                                                const std::vector<std::string> &target_object_ids,
                                                                const std::vector<std::string> &referrer_object_ids,
                                                                const std::vector<std::string> &cleanup_object_ids);
    [[nodiscard]] Result<ImageWaveDataOrphanInspection>
    inspect_wave_data_orphans(const ImageDocumentRef &target, std::string_view owner_id,
                              std::uint64_t expected_revision, std::string_view content_scope_id,
                              std::size_t maximum_candidates = 1024U);
    [[nodiscard]] Result<ImageProgramGenerationInspection>
    inspect_program_generation(const ImageDocumentRef &target, std::string_view owner_id,
                               std::uint64_t expected_revision, std::string_view content_scope_id);
    [[nodiscard]] Result<ImageProgramGenerationPlan>
    plan_program_generation(const ImageDocumentRef &target, std::string_view owner_id, std::uint64_t expected_revision,
                            std::string_view content_scope_id,
                            const std::vector<ImageProgramGenerationSelection> &selections);
    [[nodiscard]] Result<ImageProgramAssignmentCleanupInspection>
    inspect_program_assignment_cleanup(const ImageDocumentRef &target, std::string_view owner_id,
                                       std::uint64_t expected_revision, std::string_view content_scope_id);
    [[nodiscard]] Result<ImageProgramAssignmentCleanupPlan>
    plan_program_assignment_cleanup(const ImageDocumentRef &target, std::string_view owner_id,
                                    std::uint64_t expected_revision, std::string_view content_scope_id,
                                    const std::vector<ImageProgramAssignmentCleanupSelection> &selections);
    [[nodiscard]] Result<ImageSessionRead> begin_read(const ImageDocumentRef &target, std::string_view owner_id,
                                                      std::uint64_t expected_revision);
    [[nodiscard]] Result<ImageSessionRead> begin_source_read(const ImageSourceTarget &target, std::string_view owner_id,
                                                             std::uint64_t expected_revision);
    [[nodiscard]] Result<ImageSessionRead> begin_filesystem_read(std::string_view image_id, std::string_view owner_id,
                                                                 std::uint64_t expected_revision);
    [[nodiscard]] Result<ImageSessionMutation> begin_mutation(const ImageDocumentRef &target, std::string_view owner_id,
                                                              std::uint64_t expected_revision,
                                                              std::string_view operation_id = {});
    [[nodiscard]] Result<ImageSessionMutation> begin_filesystem_mutation(const ImageSourceTarget &target,
                                                                         std::string_view owner_id,
                                                                         std::uint64_t expected_revision,
                                                                         PartitionIndex partition,
                                                                         std::string_view operation_id = {});
    [[nodiscard]] Result<void>
    apply_mutation(AlterationJournalStore &journals, const ImageSessionMutation &mutation,
                   std::span<const AlterationJournalPatch> patches, const CancellationToken &cancellation = {},
                   const std::function<Result<void>()> &validate = {},
                   const std::function<void()> &on_rollback_verified = {},
                   const std::function<Result<void>(std::shared_ptr<const RandomAccessReader>)> &admit_frozen = {});
    [[nodiscard]] Result<PreparedImageSessionCommit>
    prepare_mutation_commit(std::string_view image_id, std::string_view owner_id, std::uint64_t expected_revision,
                            const CancellationToken &cancellation = {});
    [[nodiscard]] Result<void> refresh_rolled_back_mutation(std::string_view image_id, std::string_view owner_id,
                                                            std::uint64_t expected_revision);
    [[nodiscard]] ImageSessionSummary finalize_mutation_commit(PreparedImageSessionCommit prepared) noexcept;
    [[nodiscard]] Result<ImageSessionSummary> commit_mutation(std::string_view image_id, std::string_view owner_id,
                                                              std::uint64_t expected_revision,
                                                              const CancellationToken &cancellation = {});
    // Invalidate when source restoration or its refreshed access metadata is unverified.
    void abort_mutation(std::string_view image_id, std::string_view owner_id, std::uint64_t expected_revision,
                        bool invalidate_session = false) noexcept;
    [[nodiscard]] Result<void> close(std::string_view image_id, std::string_view owner_id);
    [[nodiscard]] Result<ImagePage<ImageContentItem>> content(const ImageDocumentRef &target, std::string_view owner_id,
                                                              std::size_t limit,
                                                              std::optional<std::string_view> cursor = std::nullopt,
                                                              std::optional<std::string_view> parent_id = std::nullopt);
    [[nodiscard]] Result<ImageContentScope> content_scope(const ImageDocumentRef &target, std::string_view owner_id,
                                                          std::string_view content_id);
    [[nodiscard]] Result<ImagePage<ImageObjectItem>>
    objects(const ImageDocumentRef &target, std::string_view owner_id, std::size_t limit,
            std::optional<std::string_view> cursor = std::nullopt,
            std::optional<std::string_view> object_type = std::nullopt,
            std::optional<std::string_view> content_scope_id = std::nullopt);
    [[nodiscard]] Result<nlohmann::ordered_json> object_detail(const ImageDocumentRef &target,
                                                               std::string_view owner_id, std::string_view object_id);
    [[nodiscard]] Result<ImagePage<ImageRelationshipItem>>
    relationships(const ImageDocumentRef &target, std::string_view owner_id, std::size_t limit,
                  std::optional<std::string_view> cursor = std::nullopt, ImageRelationshipFilter filter = {});
    [[nodiscard]] Result<ImageSystemProgramContexts>
    system_program_contexts(const ImageDocumentRef &target, std::string_view owner_id, std::uint8_t partition_index);
    [[nodiscard]] Result<ImageAllocationMap> allocation_map(const ImageSourceTarget &target, std::string_view owner_id,
                                                            std::uint64_t expected_revision,
                                                            std::uint8_t partition_index);
    [[nodiscard]] Result<ImagePage<ImageValidationItem>>
    validation_issues(const ImageDocumentRef &target, std::string_view owner_id, std::size_t limit,
                      std::optional<std::string_view> cursor = std::nullopt);
    [[nodiscard]] Result<ImageWaveformPreview> preview(const ImageDocumentRef &target, std::string_view owner_id,
                                                       std::string_view object_id, std::size_t bin_count,
                                                       const CancellationToken &cancellation = {});
    [[nodiscard]] Result<ImageAudition> prepare_audition(const ImageDocumentRef &target, std::string_view owner_id,
                                                         const std::vector<std::string> &object_ids,
                                                         const CancellationToken &cancellation = {},
                                                         bool stored_pcm = false);
    [[nodiscard]] Result<ImageAuditionRange> audition_range(std::string_view audition_id, std::string_view owner_id,
                                                            std::uint64_t offset, std::size_t size,
                                                            const CancellationToken &cancellation = {});
    [[nodiscard]] Result<void> delete_audition(std::string_view audition_id, std::string_view owner_id);
    void cleanup();

  private:
    [[nodiscard]] Result<ImageSessionMutation> begin_mutation_access(const ImageSourceTarget &source_target,
                                                                     std::string_view owner_id,
                                                                     std::uint64_t expected_revision,
                                                                     std::optional<PartitionIndex> filesystem_partition,
                                                                     std::string_view operation_id);
    [[nodiscard]] Result<ImageSessionRead> begin_read_access(std::string_view image_id, std::string_view owner_id,
                                                             std::uint64_t expected_revision,
                                                             std::string_view member_id, std::string_view document_id);
    friend class formats::a_series::SessionReader;
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace axk::app
