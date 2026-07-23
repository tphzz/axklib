#include "alteration_manifest_internal.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <set>
#include <string_view>
#include <type_traits>

namespace axk::detail {
namespace {

Error manifest_error(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
}

Result<void> require_text(std::string_view value, std::string_view field) {
    if (value.empty())
        return std::unexpected{manifest_error(std::string{field} + " must be a non-empty string")};
    return {};
}

Result<void> require_object_name(std::string_view value, std::string_view field) {
    if (auto valid = require_text(value, field); !valid)
        return valid;
    if (value.size() > 16U || !std::ranges::all_of(value, [](unsigned char character) { return character < 0x80U; })) {
        return std::unexpected{manifest_error(std::string{field} + " must fit 16 ASCII bytes")};
    }
    return {};
}

Result<void> require_partition_name(std::string_view value, std::string_view field) {
    if (auto valid = require_text(value, field); !valid)
        return valid;
    const auto printable =
        std::ranges::all_of(value, [](unsigned char character) { return character >= 0x20U && character <= 0x7eU; });
    if (value.size() > 16U || !printable || value.front() == ' ' || value.back() == ' ') {
        return std::unexpected{
            manifest_error(std::string{field} + " must be 1..16 printable ASCII characters without outer spaces")};
    }
    return {};
}

Result<void> validate_sample_parameters(const SampleSpec &sample) {
    if (auto valid = require_object_name(sample.name, "sample.name"); !valid)
        return valid;
    if (sample.root_key > 127U || sample.key_low > 127U || sample.key_high > 127U || sample.level > 127U)
        return std::unexpected{manifest_error("sample MIDI values must be between 0 and 127")};
    if (sample.key_high < sample.key_low)
        return std::unexpected{manifest_error("sample.key_high must not be below key_low")};
    return {};
}

Result<void> validate_direct_sample(const SampleSpec &sample) {
    if (auto valid = validate_sample_parameters(sample); !valid)
        return valid;
    if (sample.interleaved_audio_path || sample.left_waveform_name || sample.right_waveform_name ||
        sample.target_sample_rate) {
        return std::unexpected{
            manifest_error("inserted Sample must reference existing Wave Data without authored-audio fields")};
    }
    if (!sample.waveform_id)
        return std::unexpected{manifest_error("sample.waveform_id must identify the left Wave Data object")};
    if (auto valid = require_object_name(*sample.waveform_id, "sample.waveform_id"); !valid)
        return valid;
    if (sample.right_waveform_id) {
        if (auto valid = require_object_name(*sample.right_waveform_id, "sample.right_waveform_id"); !valid)
            return valid;
        if (*sample.right_waveform_id == *sample.waveform_id)
            return std::unexpected{manifest_error("sample waveform identifiers must be distinct")};
    }
    return {};
}

Result<void> validate_sample_bank(const SampleBankSpec &sample_bank) {
    if (auto valid = require_object_name(sample_bank.name, "sample_bank.name"); !valid)
        return valid;
    if (sample_bank.member_samples.empty() || sample_bank.member_samples.size() > 3U)
        return std::unexpected{manifest_error("member_samples must contain 1..3 names")};
    std::set<std::string_view> members;
    for (const auto &member : sample_bank.member_samples) {
        if (auto valid = require_object_name(member, "sample_bank.member_samples"); !valid)
            return valid;
        if (!members.insert(member).second)
            return std::unexpected{manifest_error("member_samples must be distinct")};
    }
    return {};
}

Result<void> validate_program(const ProgramSpec &program) {
    if (program.number == 0U || program.number > 128U)
        return std::unexpected{manifest_error("program.number must be between 1 and 128")};
    if (program.assignments.size() != 2U)
        return std::unexpected{manifest_error("Program requires exactly two assignments")};
    constexpr std::array<std::string_view, 2> kinds{"SBAC", "SBNK"};
    for (std::size_t index = 0; index < program.assignments.size(); ++index) {
        const auto &assignment = program.assignments[index];
        if (assignment.target_kind != kinds[index] || assignment.receive_channel != index + 1U) {
            return std::unexpected{manifest_error("Program assignments must be SBAC/channel 1 then SBNK/channel 2")};
        }
        if (auto valid = require_object_name(assignment.target_name, "program assignment target"); !valid)
            return valid;
    }
    return {};
}

Result<void> validate_volume(const VolumeSpec &volume) {
    if (auto valid = require_object_name(volume.name, "volume.name"); !valid)
        return valid;

    std::set<std::string_view> waveform_ids;
    std::set<std::string_view> waveform_names;
    for (const auto &waveform : volume.waveforms) {
        if (auto valid = require_text(waveform.id, "waveform.id"); !valid)
            return valid;
        if (!waveform_ids.insert(waveform.id).second)
            return std::unexpected{manifest_error("volume has duplicate waveform ids")};
        if (auto valid = require_object_name(waveform.name, "waveform.name"); !valid)
            return valid;
        if (!waveform_names.insert(waveform.name).second)
            return std::unexpected{manifest_error("volume has duplicate Wave Data names")};
        if (waveform.path.empty())
            return std::unexpected{manifest_error("waveform.path must be a non-empty path")};
        if (waveform.root_key > 127U || (waveform.target_sample_rate && *waveform.target_sample_rate == 0U))
            return std::unexpected{manifest_error("waveform parameters are out of range")};
    }

    std::set<std::string_view> sample_names;
    for (const auto &sample : volume.samples) {
        if (auto valid = validate_sample_parameters(sample); !valid)
            return valid;
        if (!sample_names.insert(sample.name).second)
            return std::unexpected{manifest_error("volume has duplicate Sample names")};
        const auto direct = sample.waveform_id.has_value();
        const auto interleaved = sample.interleaved_audio_path.has_value();
        if (direct == interleaved || (interleaved && sample.right_waveform_id) ||
            (direct && (sample.left_waveform_name || sample.right_waveform_name || sample.target_sample_rate))) {
            return std::unexpected{manifest_error("sample has an invalid audio source field combination")};
        }
        if (direct) {
            if (auto valid = require_text(*sample.waveform_id, "sample.waveform_id"); !valid)
                return valid;
            if (!waveform_ids.contains(*sample.waveform_id))
                return std::unexpected{manifest_error("sample references an unknown waveform")};
            if (sample.right_waveform_id && (!waveform_ids.contains(*sample.right_waveform_id) ||
                                             *sample.right_waveform_id == *sample.waveform_id)) {
                return std::unexpected{manifest_error("sample has an invalid right waveform reference")};
            }
        } else {
            if (sample.interleaved_audio_path->empty())
                return std::unexpected{manifest_error("sample.interleaved_audio_path must be a non-empty path")};
            for (const auto *name : {&sample.left_waveform_name, &sample.right_waveform_name}) {
                if (*name) {
                    if (auto valid = require_object_name(**name, "generated waveform name"); !valid)
                        return valid;
                }
            }
            if (sample.left_waveform_name && sample.right_waveform_name &&
                *sample.left_waveform_name == *sample.right_waveform_name) {
                return std::unexpected{manifest_error("generated waveform names must be distinct")};
            }
            if (sample.target_sample_rate && *sample.target_sample_rate == 0U)
                return std::unexpected{manifest_error("sample.target_sample_rate is out of range")};
        }
    }

    std::set<std::string_view> sample_bank_names;
    for (const auto &sample_bank : volume.sample_banks) {
        if (auto valid = validate_sample_bank(sample_bank); !valid)
            return valid;
        if (!sample_bank_names.insert(sample_bank.name).second)
            return std::unexpected{manifest_error("volume has duplicate Sample Bank names")};
        if (std::ranges::any_of(sample_bank.member_samples,
                                [&](const auto &member) { return !sample_names.contains(member); })) {
            return std::unexpected{manifest_error("Sample Bank references an unknown Sample")};
        }
    }

    if (volume.sample_banks.empty() != volume.programs.empty() ||
        volume.sample_banks.size() != volume.programs.size()) {
        return std::unexpected{
            manifest_error("volume requires one Program for every Sample Bank in the current writer profile")};
    }
    std::set<std::uint8_t> program_numbers;
    for (const auto &program : volume.programs) {
        if (auto valid = validate_program(program); !valid)
            return valid;
        if (!program_numbers.insert(program.number).second)
            return std::unexpected{manifest_error("volume has duplicate Program numbers")};
        if (!sample_bank_names.contains(program.assignments[0].target_name) ||
            !sample_names.contains(program.assignments[1].target_name)) {
            return std::unexpected{manifest_error("Program assignment references an unknown target")};
        }
    }
    return {};
}

Result<void> validate_operation_data(const AlterationOperationData &data) {
    return std::visit(
        [](const auto &operation) -> Result<void> {
            using T = std::decay_t<decltype(operation)>;
            if constexpr (std::same_as<T, DeleteVolumeOperation>) {
                return require_text(operation.volume_name, "volume_name");
            } else if constexpr (std::same_as<T, InsertVolumeOperation>) {
                return validate_volume(operation.volume);
            } else if constexpr (std::same_as<T, RenameVolumeOperation>) {
                if (auto valid = require_object_name(operation.volume_name, "volume_name"); !valid)
                    return valid;
                if (auto valid = require_object_name(operation.new_volume_name, "new_volume_name"); !valid)
                    return valid;
                if (operation.volume_name == operation.new_volume_name)
                    return std::unexpected{manifest_error("new_volume_name must differ")};
                return {};
            } else if constexpr (std::same_as<T, RenamePartitionOperation>) {
                if (auto valid = require_partition_name(operation.partition_name, "partition_name"); !valid)
                    return valid;
                if (auto valid = require_partition_name(operation.new_partition_name, "new_partition_name"); !valid)
                    return valid;
                if (operation.partition_name == operation.new_partition_name)
                    return std::unexpected{manifest_error("new_partition_name must differ")};
                return {};
            } else {
                if (auto valid = require_text(operation.volume_name, "volume_name"); !valid)
                    return valid;
                if constexpr (std::same_as<T, DeleteSampleOperation>) {
                    return require_object_name(operation.sample_name, "sample_name");
                } else if constexpr (std::same_as<T, InsertSampleOperation>) {
                    return validate_direct_sample(operation.sample);
                } else if constexpr (std::same_as<T, InsertWaveformOperation>) {
                    const auto &waveform = operation.waveform;
                    if (waveform.path.empty())
                        return std::unexpected{manifest_error("audio.path must be a non-empty path")};
                    if (waveform.waveform_names.empty() || waveform.waveform_names.size() > 2U) {
                        return std::unexpected{manifest_error("audio.waveform_names must contain one or two names")};
                    }
                    std::set<std::string_view> names;
                    for (const auto &name : waveform.waveform_names) {
                        if (auto valid = require_object_name(name, "audio.waveform_names"); !valid)
                            return valid;
                        if (!names.insert(name).second)
                            return std::unexpected{manifest_error("audio.waveform_names must be distinct")};
                    }
                    if (waveform.root_key > 127U)
                        return std::unexpected{manifest_error("audio.root_key must be between 0 and 127")};
                    if (waveform.target_sample_rate && *waveform.target_sample_rate == 0U)
                        return std::unexpected{manifest_error("audio.target_sample_rate is out of range")};
                    return {};
                } else if constexpr (std::same_as<T, DeleteWaveformOperation>) {
                    return require_object_name(operation.waveform_name, "waveform_name");
                } else if constexpr (std::same_as<T, RenameWaveformOperation>) {
                    if (auto valid = require_object_name(operation.waveform_name, "waveform_name"); !valid)
                        return valid;
                    if (auto valid = require_object_name(operation.new_waveform_name, "new_waveform_name"); !valid)
                        return valid;
                    if (operation.waveform_name == operation.new_waveform_name)
                        return std::unexpected{manifest_error("new_waveform_name must differ")};
                    return {};
                } else if constexpr (std::same_as<T, RenameSampleOperation>) {
                    if (auto valid = require_object_name(operation.sample_name, "sample_name"); !valid)
                        return valid;
                    if (auto valid = require_object_name(operation.new_sample_name, "new_sample_name"); !valid)
                        return valid;
                    if (operation.sample_name == operation.new_sample_name)
                        return std::unexpected{manifest_error("new_sample_name must differ")};
                    return {};
                } else if constexpr (std::same_as<T, DeleteSampleBankOperation>) {
                    return require_object_name(operation.sample_bank_name, "sample_bank_name");
                } else if constexpr (std::same_as<T, InsertSampleBankOperation>) {
                    return validate_sample_bank(operation.sample_bank);
                } else if constexpr (std::same_as<T, RenameSampleBankOperation>) {
                    if (auto valid = require_object_name(operation.sample_bank_name, "sample_bank_name"); !valid)
                        return valid;
                    if (auto valid = require_object_name(operation.new_sample_bank_name, "new_sample_bank_name");
                        !valid) {
                        return valid;
                    }
                    if (operation.sample_bank_name == operation.new_sample_bank_name)
                        return std::unexpected{manifest_error("new_sample_bank_name must differ")};
                    return {};
                } else if constexpr (std::same_as<T, DeleteProgramOperation>) {
                    if (operation.program_number == 0U || operation.program_number > 128U)
                        return std::unexpected{manifest_error("program_number must be between 1 and 128")};
                    return {};
                } else {
                    return validate_program(operation.program);
                }
            }
        },
        data);
}

} // namespace

Result<void> validate_alteration_manifest(const AlterationManifest &manifest) {
    if (manifest.schema_version != alteration_manifest_schema_version)
        return std::unexpected{manifest_error("manifest schema version must be 1.0")};
    if (manifest.operations.empty())
        return std::unexpected{manifest_error("manifest.operations must be a non-empty array")};

    std::set<std::string> seen;
    for (const auto &operation : manifest.operations) {
        if (operation.id.empty())
            return std::unexpected{manifest_error("operation id must be a non-empty string")};
        if (!seen.insert(operation.id).second)
            return std::unexpected{manifest_error("duplicate operation id")};

        const auto &selector =
            std::visit([](const auto &value) -> const PartitionSelector & { return value.partition; }, operation.data);
        if (const auto *partition = std::get_if<PartitionIndex>(&selector)) {
            if (partition->value > 7U)
                return std::unexpected{manifest_error("partition index must be 0..7")};
        } else {
            const auto &reference = std::get<OperationReference>(selector).operation_id;
            if (reference.empty() || !seen.contains(reference) || reference == operation.id)
                return std::unexpected{manifest_error("operation_ref must name an earlier operation")};
        }

        if (auto valid = validate_operation_data(operation.data); !valid)
            return valid;
    }
    return {};
}

} // namespace axk::detail
