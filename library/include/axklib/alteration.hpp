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

struct DeleteSampleOperation {
    PartitionSelector partition;
    std::string volume_name;
    std::string sample_name;
};

struct InsertSampleOperation {
    PartitionSelector partition;
    std::string volume_name;
    SampleSpec sample;
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

struct RenameSampleOperation {
    PartitionSelector partition;
    std::string volume_name;
    std::string sample_name;
    std::string new_sample_name;
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

struct RenameSampleBankOperation {
    PartitionSelector partition;
    std::string volume_name;
    std::string sample_bank_name;
    std::string new_sample_bank_name;
};

using LegacyDeleteSampleBankOperation [[deprecated("use DeleteSampleOperation for Sample (SBNK)")]] =
    DeleteSampleOperation;
using LegacyInsertSampleBankOperation [[deprecated("use InsertSampleOperation for Sample (SBNK)")]] =
    InsertSampleOperation;
using LegacyRenameSampleBankOperation [[deprecated("use RenameSampleOperation for Sample (SBNK)")]] =
    RenameSampleOperation;
using DeleteSampleBankGroupOperation [[deprecated("use DeleteSampleBankOperation for Sample Bank (SBAC)")]] =
    DeleteSampleBankOperation;
using InsertSampleBankGroupOperation [[deprecated("use InsertSampleBankOperation for Sample Bank (SBAC)")]] =
    InsertSampleBankOperation;
using RenameSampleBankGroupOperation [[deprecated("use RenameSampleBankOperation for Sample Bank (SBAC)")]] =
    RenameSampleBankOperation;

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

struct RenameVolumeOperation {
    PartitionSelector partition;
    std::string volume_name;
    std::string new_volume_name;
};

struct RenamePartitionOperation {
    PartitionSelector partition;
    std::string partition_name;
    std::string new_partition_name;
};

using AlterationOperationData =
    std::variant<DeleteVolumeOperation, InsertVolumeOperation, DeleteSampleOperation, InsertSampleOperation,
                 InsertWaveformOperation, DeleteWaveformOperation, RenameWaveformOperation, RenameSampleOperation,
                 DeleteSampleBankOperation, InsertSampleBankOperation, RenameSampleBankOperation,
                 DeleteProgramOperation, InsertProgramOperation, RenameVolumeOperation, RenamePartitionOperation>;

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
    std::uint8_t source_sample_width_bits{};
    std::uint8_t output_sample_width_bits{};
    std::uint64_t output_frames{};
    bool resampled{};
    bool quantized{};
    bool sample_width_converted{};
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

struct AlterationInspection {
    std::filesystem::path source_path;
    std::vector<OperationReport> operations;
};

AXK_AUDIO_API Result<AlterationManifest> parse_alteration_manifest(std::string_view json,
                                                                   const std::filesystem::path &base_directory = {});
AXK_AUDIO_API Result<AlterationManifest> load_alteration_manifest(const std::filesystem::path &path);
AXK_AUDIO_API Result<AlterationResult> alter_hds(const std::filesystem::path &source_path,
                                                 const AlterationManifest &manifest,
                                                 const std::filesystem::path &output_path,
                                                 const CancellationToken &cancellation = {},
                                                 ProgressSink *progress = nullptr, bool overwrite = false);
AXK_AUDIO_API Result<AlterationInspection> inspect_hds_alteration(const std::filesystem::path &source_path,
                                                                  const AlterationManifest &manifest,
                                                                  const CancellationToken &cancellation = {},
                                                                  ProgressSink *progress = nullptr);

} // namespace axk
