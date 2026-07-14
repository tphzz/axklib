#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "axklib/io.hpp"
#include "axklib/types.hpp"
#include "axklib/writer.hpp"

namespace axk {

struct OperationReference {
    std::string operation_id;
};

using PartitionSelector = std::variant<PartitionIndex, OperationReference>;

struct InsertWaveformSpec {
    std::filesystem::path path;
    std::vector<std::string> waveform_names;
    std::uint8_t root_key{};
    std::optional<std::uint32_t> target_sample_rate;
};

struct DeleteVolumeOperation {
    PartitionSelector partition;
    std::string volume_name;
};

struct InsertVolumeOperation {
    PartitionSelector partition;
    VolumeSpec volume;
};

struct DeleteSampleBankOperation {
    PartitionSelector partition;
    std::string volume_name;
    std::string sample_bank_name;
};

struct InsertSampleBankOperation {
    PartitionSelector partition;
    std::string volume_name;
    SampleBankSpec sample_bank;
};

struct InsertWaveformOperation {
    PartitionSelector partition;
    std::string volume_name;
    InsertWaveformSpec waveform;
};

struct DeleteWaveformOperation {
    PartitionSelector partition;
    std::string volume_name;
    std::string waveform_name;
};

struct RenameWaveformOperation {
    PartitionSelector partition;
    std::string volume_name;
    std::string waveform_name;
    std::string new_waveform_name;
};

struct RenameSampleBankOperation {
    PartitionSelector partition;
    std::string volume_name;
    std::string sample_bank_name;
    std::string new_sample_bank_name;
};

struct DeleteSampleBankGroupOperation {
    PartitionSelector partition;
    std::string volume_name;
    std::string sample_bank_group_name;
};

struct InsertSampleBankGroupOperation {
    PartitionSelector partition;
    std::string volume_name;
    SampleBankGroupSpec sample_bank_group;
};

struct RenameSampleBankGroupOperation {
    PartitionSelector partition;
    std::string volume_name;
    std::string sample_bank_group_name;
    std::string new_sample_bank_group_name;
};

struct DeleteProgramOperation {
    PartitionSelector partition;
    std::string volume_name;
    std::uint8_t program_number{};
};

struct InsertProgramOperation {
    PartitionSelector partition;
    std::string volume_name;
    ProgramSpec program;
};

using AlterationOperationData =
    std::variant<DeleteVolumeOperation, InsertVolumeOperation, DeleteSampleBankOperation, InsertSampleBankOperation,
                 InsertWaveformOperation, DeleteWaveformOperation, RenameWaveformOperation, RenameSampleBankOperation,
                 DeleteSampleBankGroupOperation, InsertSampleBankGroupOperation, RenameSampleBankGroupOperation,
                 DeleteProgramOperation, InsertProgramOperation>;

struct AlterationOperation {
    std::string id;
    AlterationOperationData data;
};

AXK_AUDIO_API std::string_view operation_type_name(const AlterationOperationData &operation) noexcept;

struct AlterationManifest {
    std::string schema_version;
    std::vector<AlterationOperation> operations;
};

AXK_AUDIO_API Result<std::string> serialize_alteration_manifest_template();
AXK_AUDIO_API Result<void> write_alteration_manifest_template(const std::filesystem::path &output_path,
                                                              bool overwrite = false);

struct AudioImportSummary {
    std::filesystem::path source_path;
    std::string source_format;
    std::string source_subtype;
    std::uint8_t source_channels{};
    std::uint32_t source_sample_rate{};
    std::uint32_t output_sample_rate{};
    std::uint64_t output_frames{};
    bool resampled{};
    bool quantized{};
    bool split_stereo{};
    std::string dither_algorithm;
    std::uint64_t clipped_samples{};
};

struct OperationReport {
    std::string id;
    std::string type;
    PartitionIndex partition;
    std::string volume_name;
    std::string object_name;
    std::vector<SfsId> removed_sfs_ids;
    std::vector<SfsId> inserted_sfs_ids;
    std::uint64_t freed_clusters{};
    std::uint64_t allocated_clusters{};
    std::optional<AudioImportSummary> audio_import;
};

struct AlterationResult {
    std::filesystem::path source_path;
    std::optional<std::filesystem::path> output_path;
    bool applied{};
    std::vector<OperationReport> operations;
};

struct TransactionPlan {
    std::filesystem::path source_path;
    std::vector<OperationReport> operations;
};

AXK_AUDIO_API Result<AlterationManifest> parse_alteration_manifest(std::string_view json,
                                                                   const std::filesystem::path &base_directory = {});
AXK_AUDIO_API Result<AlterationManifest> load_alteration_manifest(const std::filesystem::path &path);
AXK_AUDIO_API Result<AlterationResult> alter_hds(const std::filesystem::path &source_path,
                                                 const AlterationManifest &manifest,
                                                 const std::optional<std::filesystem::path> &output_path = {},
                                                 const CancellationToken &cancellation = {},
                                                 ProgressSink *progress = nullptr, bool overwrite = false);
AXK_AUDIO_API Result<TransactionPlan> plan_hds_alteration(const std::filesystem::path &source_path,
                                                          const AlterationManifest &manifest,
                                                          const CancellationToken &cancellation = {},
                                                          ProgressSink *progress = nullptr);

} // namespace axk
