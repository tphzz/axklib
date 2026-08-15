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

#include "axklib/application/contracts.hpp"
#include "axklib/application/filesystem.hpp"
#include "axklib/application/path_reservations.hpp"
#include "axklib/deletion.hpp"
#include "axklib/export.hpp"
#include "axklib/io.hpp"
#include "axklib/media.hpp"

namespace axk::app {

struct ImageValidationSummary {
    std::size_t info_count{};
    std::size_t warning_count{};
    std::size_t error_count{};

    [[nodiscard]] bool valid() const noexcept { return error_count == 0U; }
};

enum class ImageFloppySetStatus : std::uint8_t { single, incomplete, complete, recovery };

struct ImageFloppySetMember {
    std::uint16_t index{};
    std::string label;
    std::string marker;
};

struct ImageFloppySetSummary {
    ImageFloppySetStatus status{ImageFloppySetStatus::single};
    std::string set_label;
    std::vector<ImageFloppySetMember> members;
    std::optional<std::uint16_t> next_required_index;
};

struct ImageSessionSummary {
    std::string image_id;
    std::uint64_t revision{};
    ImageSourceRef source;
    std::vector<ImageSourceRef> companion_sources;
    std::optional<ImageFloppySetSummary> floppy_set;
    std::string format;
    std::vector<std::string> available_operations;
    std::size_t root_count{};
    std::size_t object_count{};
    std::size_t relationship_count{};
    ImageValidationSummary validation;
};

enum class CompanionSelectionKind : std::uint8_t { sources, immediate_siblings };

struct CompanionSelection {
    CompanionSelectionKind kind{CompanionSelectionKind::sources};
    std::vector<ImageSourceRef> sources;
};

struct ImageSessionMutation {
    std::string image_id;
    std::uint64_t revision{};
    FileRef source;
    std::shared_ptr<SandboxMutation> target;
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

struct ImageVolumeScopeIdentity {
    std::uint8_t partition_index{};
    std::uint32_t volume_directory_id{};
    std::string display_name;
};

struct ImageSessionRead {
    std::string image_id;
    std::uint64_t revision{};
    ImageSourceRef source;
    std::shared_ptr<const RandomAccessReader> reader;
    const MediaContainer *media{};
    std::string target_snapshot_id;
    std::vector<const ObjectSnapshot *> catalog_objects;
    std::vector<CatalogIssue> catalog_issues;
    std::unordered_map<std::string, std::string> object_keys_by_id;
    std::unordered_map<std::string, ImageVolumeScopeIdentity> volume_scopes_by_id;
    std::shared_ptr<void> lease;
};

struct ImageContentItem {
    std::string id;
    std::optional<std::string> parent_id;
    std::size_t depth{};
    std::optional<std::uint8_t> partition_index;
    std::optional<std::uint32_t> volume_directory_id;
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
    std::uint8_t root_key{};
    std::int8_t fine_tune_cents{};
    std::uint8_t loop_mode{};
    std::string loop_mode_label;
    std::uint32_t frame_count{};
    std::uint32_t loop_start_frame{};
    std::uint32_t loop_length_frames{};
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
    std::optional<WaveformMetadata> waveform;
    std::optional<SequenceMetadata> sequence;
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
    std::string model;
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
    std::uint64_t frame_count{};
    std::vector<ImagePreviewBin> bins;
};

struct ImageWaveformPreview {
    std::string object_id;
    std::uint64_t frame_count{};
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
                        std::size_t maximum_audition_clips = 256U);
    ~ImageSessionManager();
    ImageSessionManager(ImageSessionManager &&) noexcept;
    ImageSessionManager &operator=(ImageSessionManager &&) noexcept;
    ImageSessionManager(const ImageSessionManager &) = delete;
    ImageSessionManager &operator=(const ImageSessionManager &) = delete;

    [[nodiscard]] Result<ImageSessionSummary> open(const ImageSourceRef &source, std::string owner_id,
                                                   const CancellationToken &cancellation = {});
    [[nodiscard]] Result<ImageSessionSummary> attach_companions(std::string_view image_id, std::string_view owner_id,
                                                                std::uint64_t expected_revision,
                                                                const CompanionSelection &selection,
                                                                const CancellationToken &cancellation = {});
    [[nodiscard]] Result<ImageSessionSummary> inspect(std::string_view image_id, std::string_view owner_id);
    [[nodiscard]] Result<ImageObjectDeletionPlan> plan_deletion(std::string_view image_id, std::string_view owner_id,
                                                                std::uint64_t expected_revision,
                                                                const std::vector<std::string> &target_object_ids,
                                                                const std::vector<std::string> &cleanup_object_ids);
    [[nodiscard]] Result<ImageWaveDataOrphanInspection>
    inspect_wave_data_orphans(std::string_view image_id, std::string_view owner_id, std::uint64_t expected_revision,
                              std::string_view content_scope_id, std::size_t maximum_candidates = 1024U);
    [[nodiscard]] Result<ImageProgramGenerationInspection>
    inspect_program_generation(std::string_view image_id, std::string_view owner_id, std::uint64_t expected_revision,
                               std::string_view content_scope_id);
    [[nodiscard]] Result<ImageProgramGenerationPlan>
    plan_program_generation(std::string_view image_id, std::string_view owner_id, std::uint64_t expected_revision,
                            std::string_view content_scope_id,
                            const std::vector<ImageProgramGenerationSelection> &selections);
    [[nodiscard]] Result<ImageSessionRead> begin_read(std::string_view image_id, std::string_view owner_id,
                                                      std::uint64_t expected_revision);
    [[nodiscard]] Result<ImageSessionMutation> begin_mutation(std::string_view image_id, std::string_view owner_id,
                                                              std::uint64_t expected_revision);
    [[nodiscard]] Result<PreparedImageSessionCommit>
    prepare_mutation_commit(std::string_view image_id, std::string_view owner_id, std::uint64_t expected_revision,
                            const CancellationToken &cancellation = {});
    [[nodiscard]] ImageSessionSummary finalize_mutation_commit(PreparedImageSessionCommit prepared) noexcept;
    [[nodiscard]] Result<ImageSessionSummary> commit_mutation(std::string_view image_id, std::string_view owner_id,
                                                              std::uint64_t expected_revision,
                                                              const CancellationToken &cancellation = {});
    void abort_mutation(std::string_view image_id, std::string_view owner_id, std::uint64_t expected_revision) noexcept;
    [[nodiscard]] Result<void> close(std::string_view image_id, std::string_view owner_id);
    [[nodiscard]] Result<ImagePage<ImageContentItem>> content(std::string_view image_id, std::string_view owner_id,
                                                              std::size_t limit,
                                                              std::optional<std::string_view> cursor = std::nullopt,
                                                              std::optional<std::string_view> parent_id = std::nullopt);
    [[nodiscard]] Result<ImageContentScope> content_scope(std::string_view image_id, std::string_view owner_id,
                                                          std::string_view content_id);
    [[nodiscard]] Result<ImagePage<ImageObjectItem>>
    objects(std::string_view image_id, std::string_view owner_id, std::size_t limit,
            std::optional<std::string_view> cursor = std::nullopt,
            std::optional<std::string_view> object_type = std::nullopt,
            std::optional<std::string_view> content_scope_id = std::nullopt);
    [[nodiscard]] Result<ImagePage<ImageRelationshipItem>>
    relationships(std::string_view image_id, std::string_view owner_id, std::size_t limit,
                  std::optional<std::string_view> cursor = std::nullopt, ImageRelationshipFilter filter = {});
    [[nodiscard]] Result<ImageSystemProgramContexts>
    system_program_contexts(std::string_view image_id, std::string_view owner_id, std::uint8_t partition_index);
    [[nodiscard]] Result<ImagePage<ImageValidationItem>>
    validation_issues(std::string_view image_id, std::string_view owner_id, std::size_t limit,
                      std::optional<std::string_view> cursor = std::nullopt);
    [[nodiscard]] Result<ImageWaveformPreview> preview(std::string_view image_id, std::string_view owner_id,
                                                       std::string_view object_id, std::size_t bin_count,
                                                       const CancellationToken &cancellation = {});
    [[nodiscard]] Result<ImageAudition> prepare_audition(std::string_view image_id, std::string_view owner_id,
                                                         const std::vector<std::string> &object_ids,
                                                         const CancellationToken &cancellation = {});
    [[nodiscard]] Result<ImageAuditionRange> audition_range(std::string_view audition_id, std::string_view owner_id,
                                                            std::uint64_t offset, std::size_t size,
                                                            const CancellationToken &cancellation = {});
    [[nodiscard]] Result<void> delete_audition(std::string_view audition_id, std::string_view owner_id);
    void cleanup();

  private:
    [[nodiscard]] Result<ImageSessionSummary>
    open_with_companion_sources(const ImageSourceRef &source, std::string owner_id,
                                const std::vector<ImageSourceRef> &companion_sources,
                                const CancellationToken &cancellation);
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace axk::app
