#include "alteration_manifest_internal.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string_view>
#include <type_traits>

#include "axklib/sfs.hpp"
#include "axklib/writer_internal.hpp"

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

Result<void> require_program_name(std::string_view value, std::string_view field) {
    if (auto valid = require_text(value, field); !valid)
        return valid;
    const auto printable =
        std::ranges::all_of(value, [](unsigned char character) { return character >= 0x20U && character <= 0x7eU; });
    if (value.size() > 8U || !printable || value.front() == ' ' || value.back() == ' ') {
        return std::unexpected{
            manifest_error(std::string{field} + " must be 1..8 printable ASCII characters without outer spaces")};
    }
    return {};
}

constexpr bool valid_loop_mode(AudioSamplerLoopMode mode) {
    return static_cast<std::uint8_t>(mode) <= static_cast<std::uint8_t>(AudioSamplerLoopMode::reverse_one_shot);
}

constexpr bool valid_loop_settings(AudioSamplerLoopMode mode, std::uint32_t start, std::uint32_t length) {
    if (!valid_loop_mode(mode))
        return false;
    if (mode == AudioSamplerLoopMode::forward_loop || mode == AudioSamplerLoopMode::forward_loop_release)
        return length != 0U;
    return length != 0U || start == 0U;
}

constexpr std::uint8_t effective_key_low(const SampleSpec &sample) {
    return sample.key_low == sampler_original_key_low_limit ? sample.root_key : sample.key_low;
}

constexpr std::uint8_t effective_key_high(const SampleSpec &sample) {
    return sample.key_high == sampler_original_key_high_limit ? sample.root_key : sample.key_high;
}

Result<void> validate_sample_parameters(const SampleSpec &sample) {
    if (auto valid = require_object_name(sample.name, "sample.name"); !valid)
        return valid;
    if (sample.root_key > 127U || (sample.key_low > 127U && sample.key_low != sampler_original_key_low_limit) ||
        sample.key_high > sampler_original_key_high_limit || sample.level > 127U || sample.velocity_low > 127U ||
        sample.velocity_high > 127U || sample.fine_tune_cents < -63 || sample.fine_tune_cents > 63 ||
        sample.expand_detune < -7 || sample.expand_detune > 7 || sample.expand_dephase < -63 ||
        sample.expand_dephase > 63 || sample.expand_width < -63 || sample.expand_width > 63)
        return std::unexpected{manifest_error("sample parameters are outside their supported ranges")};
    if (effective_key_high(sample) < effective_key_low(sample) || sample.velocity_high < sample.velocity_low)
        return std::unexpected{manifest_error("sample key and velocity ranges must be ordered")};
    if (!valid_loop_settings(sample.loop_mode, sample.loop_start_frame, sample.loop_length_frames))
        return std::unexpected{manifest_error("sample loop settings are invalid")};
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
        if (sample.expand_detune != 0 || sample.expand_dephase != 0)
            return std::unexpected{manifest_error("stereo Sample cannot use expanded-mono controls")};
    }
    return {};
}

Result<void> validate_sample_bank_parameter_overrides(const SampleBankParameterOverrides &overrides) {
    const auto root_key = overrides.root_key.value_or(60U);
    const auto key_low = overrides.key_low.value_or(0U);
    const auto key_high = overrides.key_high.value_or(127U);
    const auto velocity_low = overrides.velocity_low.value_or(0U);
    const auto velocity_high = overrides.velocity_high.value_or(127U);
    const auto effective_low = key_low == sampler_original_key_low_limit ? root_key : key_low;
    const auto effective_high = key_high == sampler_original_key_high_limit ? root_key : key_high;
    if (root_key > 127U || (key_low > 127U && key_low != sampler_original_key_low_limit) ||
        key_high > sampler_original_key_high_limit || effective_high < effective_low ||
        overrides.level.value_or(100U) > 127U || overrides.fine_tune_cents.value_or(0) < -63 ||
        overrides.fine_tune_cents.value_or(0) > 63 || velocity_low > 127U || velocity_high > 127U ||
        velocity_high < velocity_low || overrides.expand_detune.value_or(0) < -7 ||
        overrides.expand_detune.value_or(0) > 7 || overrides.expand_dephase.value_or(0) < -63 ||
        overrides.expand_dephase.value_or(0) > 63 || overrides.expand_width.value_or(63) < -63 ||
        overrides.expand_width.value_or(63) > 63) {
        return std::unexpected{manifest_error("Sample Bank parameter overrides are outside their supported ranges")};
    }
    return {};
}

Result<void> validate_sample_bank(const SampleBankSpec &sample_bank) {
    if (auto valid = require_object_name(sample_bank.name, "sample_bank.name"); !valid)
        return valid;
    if (sample_bank.member_samples.empty() || sample_bank.member_samples.size() > maximum_sample_bank_members)
        return std::unexpected{manifest_error("member_samples must contain 1..127 names")};
    std::set<std::string_view> members;
    for (const auto &member : sample_bank.member_samples) {
        if (auto valid = require_object_name(member, "sample_bank.member_samples"); !valid)
            return valid;
        if (!members.insert(member).second)
            return std::unexpected{manifest_error("member_samples must be distinct")};
    }
    if (sample_bank.parameter_overrides) {
        if (auto valid = validate_sample_bank_parameter_overrides(*sample_bank.parameter_overrides); !valid)
            return valid;
    }
    return {};
}

Result<void> validate_program_fields(const ProgramSpec &program) {
    if (program.number == 0U || program.number > 128U)
        return std::unexpected{manifest_error("program.number must be between 1 and 128")};
    if (auto valid = require_program_name(program.name, "program.name"); !valid)
        return valid;
    if (program.assignments.empty() || program.assignments.size() > maximum_program_assignments) {
        return std::unexpected{manifest_error("program.assignments must contain 1..16 assignments")};
    }
    for (const auto &assignment : program.assignments) {
        if (assignment.target_kind != "SBAC" && assignment.target_kind != "SBNK")
            return std::unexpected{manifest_error("Program assignment target must be SBAC or SBNK")};
        if (auto valid = require_object_name(assignment.target_name, "program assignment target"); !valid)
            return valid;
        if (assignment.receive_mode == ProgramReceiveMode::midi_channel &&
            (assignment.receive_channel == 0U || assignment.receive_channel > 16U)) {
            return std::unexpected{manifest_error("MIDI_CHANNEL Program assignment requires channel 1..16")};
        }
        if (assignment.receive_mode == ProgramReceiveMode::sample && assignment.receive_channel != 0U)
            return std::unexpected{manifest_error("SAMPLE Program assignment must not specify a MIDI channel")};
    }
    return {};
}

Result<void> validate_authored_program(const ProgramSpec &program) {
    if (auto valid = validate_program_fields(program); !valid)
        return valid;
    if (program.assignments.size() != 2U || program.assignments[0].target_kind != "SBAC" ||
        program.assignments[0].receive_mode != ProgramReceiveMode::midi_channel ||
        program.assignments[0].receive_channel != 1U || program.assignments[1].target_kind != "SBNK" ||
        program.assignments[1].receive_mode != ProgramReceiveMode::midi_channel ||
        program.assignments[1].receive_channel != 2U) {
        return std::unexpected{manifest_error("authored Program assignments must be SBAC/channel 1 then "
                                              "SBNK/channel 2")};
    }
    return {};
}

Result<void> validate_inserted_program(const ProgramSpec &program) { return validate_program_fields(program); }

Result<void> validate_volume(const VolumeSpec &volume) {
    if (auto valid = require_object_name(volume.name, "volume.name"); !valid)
        return valid;
    if (is_partition_support_root_entry(volume.name))
        return std::unexpected{manifest_error("volume.name PRF3 is reserved for partition support files")};

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
        if (waveform.root_key > 127U || waveform.fine_tune_cents < -63 || waveform.fine_tune_cents > 63 ||
            (waveform.target_sample_rate && *waveform.target_sample_rate == 0U) ||
            !valid_loop_settings(waveform.loop_mode, waveform.loop_start_frame, waveform.loop_length_frames))
            return std::unexpected{manifest_error("waveform parameters are out of range")};
    }

    std::set<std::string_view> sample_names;
    std::map<std::string_view, const SampleSpec *> sample_specs;
    for (const auto &sample : volume.samples) {
        if (auto valid = validate_sample_parameters(sample); !valid)
            return valid;
        if (!sample_names.insert(sample.name).second)
            return std::unexpected{manifest_error("volume has duplicate Sample names")};
        sample_specs.emplace(sample.name, &sample);
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
            if (sample.right_waveform_id && (sample.expand_detune != 0 || sample.expand_dephase != 0))
                return std::unexpected{manifest_error("stereo Sample cannot use expanded-mono controls")};
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
            if (sample.expand_detune != 0 || sample.expand_dephase != 0)
                return std::unexpected{manifest_error("interleaved stereo Sample cannot use expanded-mono controls")};
        }
    }

    std::set<std::string_view> sample_bank_names;
    std::set<std::string_view> banked_samples;
    for (const auto &sample_bank : volume.sample_banks) {
        if (auto valid = validate_sample_bank(sample_bank); !valid)
            return valid;
        if (!sample_bank_names.insert(sample_bank.name).second)
            return std::unexpected{manifest_error("volume has duplicate Sample Bank names")};
        if (std::ranges::any_of(sample_bank.member_samples,
                                [&](const auto &member) { return !sample_names.contains(member); })) {
            return std::unexpected{manifest_error("Sample Bank references an unknown Sample")};
        }
        for (const auto &member_name : sample_bank.member_samples) {
            if (!banked_samples.insert(member_name).second)
                return std::unexpected{manifest_error("Sample cannot belong to multiple Sample Banks")};
            if (!sample_bank.parameter_overrides)
                continue;
            const auto effective =
                apply_sample_bank_parameter_overrides(*sample_specs.at(member_name), *sample_bank.parameter_overrides);
            if (auto valid = validate_sample_parameters(effective); !valid)
                return valid;
            if ((effective.right_waveform_id || effective.interleaved_audio_path) &&
                (effective.expand_detune != 0 || effective.expand_dephase != 0)) {
                return std::unexpected{
                    manifest_error("stereo Sample cannot receive expanded-mono Sample Bank controls")};
            }
        }
    }

    if (volume.sample_banks.empty() != volume.programs.empty() ||
        volume.sample_banks.size() != volume.programs.size()) {
        return std::unexpected{
            manifest_error("volume requires one Program for every Sample Bank in the current writer profile")};
    }
    std::set<std::uint8_t> program_numbers;
    for (const auto &program : volume.programs) {
        if (auto valid = validate_authored_program(program); !valid)
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
                if (auto valid = require_text(operation.volume_name, "volume_name"); !valid)
                    return valid;
                if (is_partition_support_root_entry(operation.volume_name))
                    return std::unexpected{manifest_error("volume_name PRF3 is reserved for partition support files")};
                return {};
            } else if constexpr (std::same_as<T, InsertVolumeOperation>) {
                return validate_volume(operation.volume);
            } else if constexpr (std::same_as<T, RenameVolumeOperation>) {
                if (auto valid = require_object_name(operation.volume_name, "volume_name"); !valid)
                    return valid;
                if (auto valid = require_object_name(operation.new_volume_name, "new_volume_name"); !valid)
                    return valid;
                if (is_partition_support_root_entry(operation.volume_name) ||
                    is_partition_support_root_entry(operation.new_volume_name)) {
                    return std::unexpected{manifest_error("PRF3 is reserved for partition support files")};
                }
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
            } else if constexpr (std::same_as<T, RepairObjectPlacementsOperation>) {
                if (auto valid = require_text(operation.volume_name, "volume_name"); !valid)
                    return valid;
                if (operation.object_sfs_ids.empty() || operation.object_sfs_ids.size() > 4096U)
                    return std::unexpected{manifest_error("object_sfs_ids must contain 1..4096 SFS IDs")};
                std::set<std::uint32_t> ids;
                for (const auto id : operation.object_sfs_ids) {
                    if (id.value <= 2U || !ids.insert(id.value).second)
                        return std::unexpected{manifest_error("object_sfs_ids must contain unique object SFS IDs")};
                }
                return {};
            } else if constexpr (std::same_as<T, ImportTx16wDiskSetOperation>) {
                if (auto valid = require_object_name(operation.volume_name, "volume_name"); !valid)
                    return valid;
                if (operation.disk_paths.empty() || operation.disk_paths.size() > 32U)
                    return std::unexpected{manifest_error("disk_paths must contain 1..32 paths")};
                if (std::ranges::any_of(operation.disk_paths, &std::filesystem::path::empty))
                    return std::unexpected{manifest_error("disk_paths must contain non-empty paths")};
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
                    if (waveform.fine_tune_cents < -63 || waveform.fine_tune_cents > 63 ||
                        !valid_loop_settings(waveform.loop_mode, waveform.loop_start_frame,
                                             waveform.loop_length_frames))
                        return std::unexpected{manifest_error("audio sampler settings are invalid")};
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
                } else if constexpr (std::same_as<T, AssignSampleBankMembersOperation>) {
                    if (auto valid = require_object_name(operation.sample_bank_name, "sample_bank_name"); !valid)
                        return valid;
                    if (operation.sample_names.empty() || operation.sample_names.size() > maximum_sample_bank_members)
                        return std::unexpected{manifest_error("sample_names must contain 1..127 names")};
                    std::set<std::string_view> names;
                    for (const auto &name : operation.sample_names) {
                        if (auto valid = require_object_name(name, "sample_names"); !valid)
                            return valid;
                        if (!names.insert(name).second)
                            return std::unexpected{manifest_error("sample_names must be distinct")};
                    }
                    return {};
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
                } else if constexpr (std::same_as<T, RenameProgramOperation>) {
                    if (operation.program_number == 0U || operation.program_number > 128U)
                        return std::unexpected{manifest_error("program_number must be between 1 and 128")};
                    return require_program_name(operation.new_program_name, "new_program_name");
                } else if constexpr (std::same_as<T, ClearProgramAssignmentsOperation>) {
                    if (operation.program_number == 0U || operation.program_number > 128U)
                        return std::unexpected{manifest_error("program_number must be between 1 and 128")};
                    if (operation.assignment_ordinals.empty() ||
                        operation.assignment_ordinals.size() > maximum_program_assignments) {
                        return std::unexpected{manifest_error("assignment_ordinals must contain 1..16 ordinals")};
                    }
                    std::set<std::uint8_t> ordinals;
                    for (const auto ordinal : operation.assignment_ordinals) {
                        if (ordinal >= maximum_program_assignments || !ordinals.insert(ordinal).second) {
                            return std::unexpected{
                                manifest_error("assignment_ordinals must contain distinct values between 0 and 15")};
                        }
                    }
                    return {};
                } else if constexpr (std::same_as<T, DeleteSequenceOperation>) {
                    return require_object_name(operation.sequence_name, "sequence_name");
                } else if constexpr (std::same_as<T, InsertSequenceOperation>) {
                    if (auto valid = require_object_name(operation.sequence.name, "sequence.name"); !valid)
                        return valid;
                    if (operation.sequence.midi_path.empty())
                        return std::unexpected{manifest_error("sequence.midi_path must be a non-empty path")};
                    return {};
                } else if constexpr (std::same_as<T, RenameSequenceOperation>) {
                    if (auto valid = require_object_name(operation.sequence_name, "sequence_name"); !valid)
                        return valid;
                    if (auto valid = require_object_name(operation.new_sequence_name, "new_sequence_name"); !valid)
                        return valid;
                    if (operation.sequence_name == operation.new_sequence_name)
                        return std::unexpected{manifest_error("new_sequence_name must differ")};
                    return {};
                } else {
                    return validate_inserted_program(operation.program);
                }
            }
        },
        data);
}

Result<void> validate_placement_repair_transaction(const AlterationManifest &manifest) {
    const auto repair_count = std::ranges::count_if(manifest.operations, [](const AlterationOperation &operation) {
        return std::holds_alternative<RepairObjectPlacementsOperation>(operation.data);
    });
    if (repair_count == 0U)
        return {};

    std::optional<PartitionIndex> partition;
    std::set<std::uint32_t> repaired_ids;
    std::size_t inserted_volume_count{};
    bool repair_seen{};
    for (const auto &operation : manifest.operations) {
        const auto *repair = std::get_if<RepairObjectPlacementsOperation>(&operation.data);
        const auto *insert = std::get_if<InsertVolumeOperation>(&operation.data);
        if (!repair && !insert) {
            return std::unexpected{
                manifest_error("placement repair transactions may only create an empty recovery volume")};
        }
        const auto &selector = repair ? repair->partition : insert->partition;
        const auto *operation_partition = std::get_if<PartitionIndex>(&selector);
        if (!operation_partition)
            return std::unexpected{manifest_error("placement repair transactions require an explicit partition")};
        if (partition && *partition != *operation_partition)
            return std::unexpected{manifest_error("placement repair transaction operations must use one partition")};
        partition = *operation_partition;

        if (insert) {
            ++inserted_volume_count;
            if (inserted_volume_count > 1U || repair_seen || !insert->volume.waveforms.empty() ||
                !insert->volume.samples.empty() || !insert->volume.sample_banks.empty() ||
                !insert->volume.programs.empty()) {
                return std::unexpected{
                    manifest_error("placement repair may create one empty recovery volume before repair operations")};
            }
            continue;
        }

        repair_seen = true;
        for (const auto id : repair->object_sfs_ids) {
            if (!repaired_ids.insert(id.value).second)
                return std::unexpected{manifest_error("placement repair object SFS IDs must be globally unique")};
        }
    }
    return {};
}

} // namespace

Result<void> validate_alteration_manifest(const AlterationManifest &manifest) {
    if (manifest.schema_version != alteration_manifest_schema_version)
        return std::unexpected{manifest_error("manifest schema version must be 1.0")};
    if (manifest.operations.empty())
        return std::unexpected{manifest_error("manifest.operations must be a non-empty array")};
    if (auto valid = validate_placement_repair_transaction(manifest); !valid)
        return valid;

    std::set<std::string> seen;
    for (const auto &operation : manifest.operations) {
        if (operation.id.empty())
            return std::unexpected{manifest_error("operation id must be a non-empty string")};
        if (!seen.insert(operation.id).second)
            return std::unexpected{manifest_error("duplicate operation id")};

        auto selector_valid = std::visit(
            [&](const auto &value) -> Result<void> {
                if (const auto *partition = std::get_if<PartitionIndex>(&value.partition)) {
                    if (partition->value > 7U)
                        return std::unexpected{manifest_error("partition index must be 0..7")};
                    return {};
                }

                const auto &reference = std::get<OperationReference>(value.partition).operation_id;
                if (reference.empty() || !seen.contains(reference) || reference == operation.id)
                    return std::unexpected{manifest_error("operation_ref must name an earlier operation")};
                return {};
            },
            operation.data);
        if (!selector_valid)
            return selector_valid;

        if (auto valid = validate_operation_data(operation.data); !valid)
            return valid;
    }
    return {};
}

} // namespace axk::detail
