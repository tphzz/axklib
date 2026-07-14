#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/export.hpp"
#include "axklib/relationship.hpp"
#include "axklib/sfs.hpp"

namespace axk {

class MediaContainer;

struct PhysicalWaveformExport {
    std::string object_key;
    std::string display_name;
    std::filesystem::path relative_wav_path;
    Waveform waveform;
};

struct BankMemberExport {
    std::string role;
    std::string waveform_key;
    std::filesystem::path relative_wav_path;
    RelationshipQuality quality{RelationshipQuality::unknown};
};

struct BankParameterContext {
    std::string object_key;
    std::string display_name;
    std::string relationship_type;
    CurrentSbnk decoded;
};

struct SampleBankExport {
    std::string object_key;
    std::string display_name;
    std::vector<BankMemberExport> members;
    std::optional<std::filesystem::path> rendered_wav_path;
    std::optional<StereoRenderDecision> stereo_decision;
    std::uint8_t key_low{};
    std::uint8_t key_high{};
    std::int8_t coarse_tune{};
    CurrentSbnk decoded;
    std::vector<BankParameterContext> parameter_contexts;
};

struct SampleBankGroupExport {
    std::string object_key;
    std::string display_name;
    std::vector<std::string> member_bank_keys;
    std::vector<std::string> relationship_bank_keys;
};

struct ProgramExport {
    std::string object_key;
    std::string display_name;
    std::vector<std::string> assignment_target_keys;
};

struct VolumeExport {
    PartitionIndex partition;
    std::string partition_name;
    std::string volume_name;
    std::filesystem::path relative_root;
    std::vector<PhysicalWaveformExport> waveforms;
    std::vector<SampleBankExport> sample_banks;
    std::vector<SampleBankGroupExport> sample_bank_groups;
    std::vector<ProgramExport> programs;
};

struct ExportPlan {
    std::filesystem::path source_path;
    std::vector<VolumeExport> volumes;
    std::vector<std::string> decode_errors;
};

struct ExportResult {
    std::vector<std::filesystem::path> written_files;
    std::vector<std::string> warnings;
};

struct SfzExportResult {
    std::vector<std::filesystem::path> written_files;
    std::vector<std::string> warnings;
};

AXK_API Result<ExportPlan>
build_export_plan(const Container &container, const ObjectCatalog &catalog,
                  const RelationshipGraph &graph,
                  const CancellationToken &cancellation = {});
AXK_API Result<ExportPlan>
build_export_plan(const MediaContainer &container, const ObjectCatalog &catalog,
                  const RelationshipGraph &graph,
                  const CancellationToken &cancellation = {});
AXK_API Result<ExportResult> write_export_audio(
    const ExportPlan &plan, const std::filesystem::path &output_directory,
    bool overwrite = false, const CancellationToken &cancellation = {});
AXK_API Result<SfzExportResult>
write_sfz(const ExportPlan &plan, const std::filesystem::path &output_directory,
          bool overwrite = false);

} // namespace axk
