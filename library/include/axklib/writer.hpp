#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/io.hpp"

namespace axk {

inline constexpr std::uint64_t minimum_hds_size = 1'048'576;
inline constexpr std::uint64_t maximum_hds_size = 2'147'483'648;
inline constexpr std::uint64_t maximum_wave_data_frames_per_channel = 1ULL << 24U;
inline constexpr std::uint64_t maximum_audio_source_frames_per_channel = maximum_wave_data_frames_per_channel;
inline constexpr std::uint64_t maximum_audio_decoded_source_bytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t maximum_wave_data_pcm16_bytes_per_channel =
    maximum_wave_data_frames_per_channel * sizeof(std::int16_t);
inline constexpr std::array<std::uint32_t, 12> supported_sampler_sample_rates{
    4'000U, 5'512U, 6'000U, 8'000U, 11'025U, 12'000U, 16'000U, 22'050U, 24'000U, 32'000U, 44'100U, 48'000U};
inline constexpr std::uint32_t default_sampler_sample_rate = 44'100U;
inline constexpr std::uint8_t sampler_output_sample_width_bits = 16U;
inline constexpr std::array<std::uint8_t, 1> supported_sampler_output_sample_widths_bits{
    sampler_output_sample_width_bits};
inline constexpr std::string_view sampler_sample_width_policy = "PRESERVE_PCM16_EXPAND_PCM8";

struct WaveformSpec {
    std::string id;
    std::string name;
    std::filesystem::path path;
    std::uint8_t root_key{};
    std::optional<std::uint32_t> target_sample_rate;
};

struct SampleSpec {
    std::string name;
    std::optional<std::string> waveform_id;
    std::optional<std::string> right_waveform_id;
    std::optional<std::filesystem::path> interleaved_audio_path;
    std::optional<std::string> left_waveform_name;
    std::optional<std::string> right_waveform_name;
    std::optional<std::uint32_t> target_sample_rate;
    std::uint8_t root_key{};
    std::uint8_t key_low{};
    std::uint8_t key_high{};
    std::uint8_t level{127};
};

struct SampleBankSpec {
    std::string name;
    std::vector<std::string> member_samples;
};

using LegacySampleBankSpec [[deprecated("use SampleSpec for Sample (SBNK)")]] = SampleSpec;
using SampleBankGroupSpec [[deprecated("use SampleBankSpec for Sample Bank (SBAC)")]] = SampleBankSpec;

struct ProgramAssignmentSpec {
    std::string target_kind;
    std::string target_name;
    std::uint8_t receive_channel{};
};

struct ProgramSpec {
    std::uint8_t number{};
    std::vector<ProgramAssignmentSpec> assignments;
};

struct VolumeSpec {
    std::string name;
    std::vector<WaveformSpec> waveforms;
    std::vector<SampleSpec> samples;
    std::vector<SampleBankSpec> sample_banks;
    std::vector<ProgramSpec> programs;
};

struct PartitionSpec {
    std::string name;
    std::vector<VolumeSpec> volumes;
};

struct HdsBuildManifest {
    std::string schema_version;
    std::uint64_t size_bytes{};
    std::vector<PartitionSpec> partitions;
};

enum class MediaImageFormat : std::uint8_t { fat12_floppy, iso9660 };
enum class SavedObjectSelection : std::uint8_t { roots, all };
enum class BuildManifestKind : std::uint8_t { hds, fat12_floppy, iso9660 };

struct SavedObjectTransferSpec {
    std::filesystem::path source_path;
    std::vector<std::string> root_object_keys;
    SavedObjectSelection selection{SavedObjectSelection::roots};
};

struct MediaBuildManifest {
    std::string schema_version;
    MediaImageFormat format{MediaImageFormat::fat12_floppy};
    std::optional<SavedObjectTransferSpec> transfer;
    std::optional<VolumeSpec> authored_volume;
    std::string iso_volume_id{"AXKLIB"};
    std::string raw_group{"GROUP"};
    std::string group_name{"AXKLIB"};
    std::string raw_volume{"F001"};
    std::string volume_name{"AXKLIB"};
};

struct AudioImportOptions {
    std::uint8_t expected_channels{1};
    std::optional<std::uint32_t> target_sample_rate;
};

struct AudioImportIssue {
    std::string code;
    std::string message;
    bool fatal{true};
};

struct AudioSourceInfo {
    std::string source_format;
    std::string source_subtype;
    std::uint8_t channels{};
    std::uint64_t frame_count{};
    std::uint32_t source_sample_rate{};
    std::uint32_t output_sample_rate{};
    std::uint8_t source_sample_width_bits{};
    std::uint8_t output_sample_width_bits{sampler_output_sample_width_bits};
    bool resampled{};
    bool quantized{};
    bool sample_width_converted{};
    std::string dither_algorithm;
    std::uint64_t projected_output_frame_count{};
    std::uint64_t projected_output_bytes_per_channel{};
    std::uint64_t projected_output_bytes_total{};
    std::uint64_t maximum_output_frame_count_per_channel{maximum_wave_data_frames_per_channel};
    std::uint64_t maximum_output_bytes_per_channel{maximum_wave_data_pcm16_bytes_per_channel};
    bool valid{true};
    std::vector<AudioImportIssue> issues;
};

struct ImportedAudio {
    std::filesystem::path source_path;
    std::string source_format;
    std::string source_subtype;
    std::uint8_t source_channels{};
    std::uint32_t source_sample_rate{};
    std::uint32_t output_sample_rate{};
    std::uint8_t source_sample_width_bits{};
    std::uint8_t output_sample_width_bits{sampler_output_sample_width_bits};
    std::uint64_t output_frames{};
    std::vector<std::vector<std::byte>> pcm_channels;
    bool resampled{};
    bool quantized{};
    bool sample_width_converted{};
    // Empty for exact PCM16 imports and exact PCM8 expansion; otherwise
    // identifies the reproducible policy used.
    std::string dither_algorithm;
    std::uint64_t clipped_samples{};
};

struct PartitionGeometry {
    std::uint8_t index{};
    std::uint64_t start_sector{};
    std::uint64_t slot_sector_count{};
    std::uint64_t filesystem_sector_count{};
    std::uint64_t cluster_count{};
    std::uint64_t bitmap_cluster{};
    std::uint64_t bitmap_cluster_count{};
    std::uint64_t directory_index_cluster{};
    std::uint64_t first_payload_cluster{};
};

struct WrittenPartitionLayout {
    PartitionGeometry geometry;
    std::string name;
    std::uint64_t allocated_cluster_count{};
    std::uint64_t free_cluster_count{};
    std::uint64_t free_bytes{};
    std::uint64_t sampler_visible_free_kib{};
};

struct WrittenImageLayout {
    std::filesystem::path path;
    std::uint64_t size_bytes{};
    std::vector<WrittenPartitionLayout> partitions;
    std::uint64_t unused_tail_sectors{};
};

struct WrittenMediaImage {
    std::filesystem::path path;
    MediaImageFormat format{MediaImageFormat::fat12_floppy};
    std::uint64_t size_bytes{};
    std::size_t object_count{};
};

struct HdsBuildPlanSummary {
    std::uint64_t size_bytes{};
    std::size_t partition_count{};
    std::size_t object_count{};
    std::vector<PartitionGeometry> partitions;
};

enum class HdsCreationProfileId : std::uint8_t { floppy_scale, cd_r_650, cd_r_700, hds_1_gib, hds_2_gib };

struct HdsCreationPartitionOption {
    std::uint8_t partition_count{};
    std::vector<PartitionGeometry> partitions;
    std::uint64_t unused_tail_sectors{};
};

struct HdsCreationProfile {
    HdsCreationProfileId id{HdsCreationProfileId::floppy_scale};
    std::uint64_t size_bytes{};
    std::uint8_t default_partition_count{};
    std::vector<HdsCreationPartitionOption> partition_options;
};

struct HdsCreationRequest {
    HdsCreationProfileId profile_id{HdsCreationProfileId::floppy_scale};
    std::uint8_t partition_count{};
};

struct HdsCreationPlan {
    HdsCreationProfileId profile_id{HdsCreationProfileId::floppy_scale};
    HdsBuildManifest manifest;
    HdsBuildPlanSummary summary;
    std::uint64_t unused_tail_sectors{};
};

struct MediaBuildPlanSummary {
    MediaImageFormat format{MediaImageFormat::fat12_floppy};
    std::size_t object_count{};
};

AXK_AUDIO_API Result<HdsBuildManifest> parse_hds_build_manifest(std::string_view json,
                                                                const std::filesystem::path &base_directory = {});
AXK_AUDIO_API Result<HdsBuildManifest> load_hds_build_manifest(const std::filesystem::path &path);
AXK_AUDIO_API Result<MediaBuildManifest> parse_media_build_manifest(std::string_view json,
                                                                    const std::filesystem::path &base_directory = {});
AXK_AUDIO_API Result<MediaBuildManifest> load_media_build_manifest(const std::filesystem::path &path);
AXK_AUDIO_API Result<std::string> serialize_build_manifest_template(BuildManifestKind kind);
AXK_AUDIO_API Result<void>
write_build_manifest_template(BuildManifestKind kind, const std::filesystem::path &output_path, bool overwrite = false);
AXK_AUDIO_API Result<std::vector<PartitionGeometry>> plan_hds_geometry(const HdsBuildManifest &manifest);
AXK_AUDIO_API std::string_view hds_creation_profile_id(HdsCreationProfileId id);
AXK_AUDIO_API Result<HdsCreationProfileId> parse_hds_creation_profile_id(std::string_view id);
AXK_AUDIO_API const std::vector<HdsCreationProfile> &hds_creation_profiles();
AXK_AUDIO_API Result<HdsCreationPlan> plan_hds_creation(const HdsCreationRequest &request,
                                                        const CancellationToken &cancellation = {});
AXK_AUDIO_API Result<HdsBuildPlanSummary> plan_hds_build(const HdsBuildManifest &manifest,
                                                         const CancellationToken &cancellation = {});
AXK_AUDIO_API Result<MediaBuildPlanSummary> plan_media_build(const MediaBuildManifest &manifest,
                                                             const CancellationToken &cancellation = {});
AXK_AUDIO_API Result<std::uint32_t> choose_sampler_sample_rate(std::uint32_t source_rate,
                                                               std::optional<std::uint32_t> target_sample_rate = {});
AXK_AUDIO_API Result<AudioSourceInfo> inspect_sampler_audio(const std::filesystem::path &path,
                                                            std::optional<std::uint32_t> target_sample_rate = {});
AXK_AUDIO_API Result<AudioSourceInfo> inspect_sampler_audio(const RandomAccessReader &reader,
                                                            std::optional<std::uint32_t> target_sample_rate = {});
AXK_AUDIO_API Result<ImportedAudio> import_sampler_audio(const std::filesystem::path &path,
                                                         const AudioImportOptions &options);
AXK_AUDIO_API Result<ImportedAudio> import_sampler_audio(const RandomAccessReader &reader,
                                                         const AudioImportOptions &options);
AXK_AUDIO_API Result<WrittenImageLayout> write_hds_image(const HdsBuildManifest &manifest,
                                                         const std::filesystem::path &output_path,
                                                         bool overwrite = false,
                                                         const CancellationToken &cancellation = {});
AXK_AUDIO_API Result<WrittenMediaImage> write_media_image(const MediaBuildManifest &manifest,
                                                          const std::filesystem::path &output_path,
                                                          bool overwrite = false,
                                                          const CancellationToken &cancellation = {});

} // namespace axk
