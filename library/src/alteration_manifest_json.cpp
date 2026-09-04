#include "axklib/alteration.hpp"

#include "axklib/utf8.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <ranges>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "alteration_manifest_internal.hpp"
#include "alteration_manifest_placement.hpp"
#include "alteration_manifest_program.hpp"
#include "alteration_manifest_sequence.hpp"

namespace axk {
namespace {
using Json = nlohmann::json;
Error transaction_error(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
}
Result<std::string> required_text(const Json &row, std::string_view field, std::string_view context) {
    if (!row.contains(field) || !row[field].is_string() || row[field].get_ref<const std::string &>().empty()) {
        return std::unexpected{
            transaction_error(std::string{context} + "." + std::string{field} + " must be a non-empty string")};
    }
    return row[field].get<std::string>();
}
Result<void> exact_fields(const Json &row, std::initializer_list<std::string_view> expected, std::string_view context) {
    if (!row.is_object() || row.size() != expected.size()) {
        return std::unexpected{transaction_error(std::string{context} + " has invalid fields")};
    }
    for (const auto field : expected) {
        if (!row.contains(field)) {
            return std::unexpected{transaction_error(std::string{context} + " is missing field " + std::string{field})};
        }
    }
    return {};
}
Result<std::uint8_t> midi_value(const Json &row, std::string_view field, std::string_view context,
                                std::uint8_t default_value, bool required, std::uint8_t maximum = 127U) {
    if (!row.contains(field)) {
        if (!required)
            return default_value;
        return std::unexpected{transaction_error(std::string{context} + " is missing field " + std::string{field})};
    }
    if (!row[field].is_number_integer()) {
        return std::unexpected{
            transaction_error(std::string{context} + "." + std::string{field} + " must be an integer")};
    }
    const auto value = row[field].get<int>();
    if (value < 0 || value > maximum) {
        return std::unexpected{
            transaction_error(std::string{context} + "." + std::string{field} + " is outside its supported range")};
    }
    return static_cast<std::uint8_t>(value);
}
Result<std::int64_t> bounded_integer(const Json &row, std::string_view field, std::string_view context,
                                     std::int64_t minimum, std::int64_t maximum, std::int64_t default_value) {
    if (!row.contains(field))
        return default_value;
    if (!row[field].is_number_integer())
        return std::unexpected{
            transaction_error(std::string{context} + "." + std::string{field} + " must be an integer")};
    const auto value = row[field].get<std::int64_t>();
    if (value < minimum || value > maximum)
        return std::unexpected{transaction_error(std::string{context} + "." + std::string{field} + " is out of range")};
    return value;
}
Result<std::uint8_t> program_value(const Json &row, std::string_view field, std::string_view context) {
    if (!row.contains(field) || !row[field].is_number_integer()) {
        return std::unexpected{
            transaction_error(std::string{context} + "." + std::string{field} + " must be an integer")};
    }
    const auto value = row[field].get<int>();
    if (value < 1 || value > 128) {
        return std::unexpected{
            transaction_error(std::string{context} + "." + std::string{field} + " must be between 1 and 128")};
    }
    return static_cast<std::uint8_t>(value);
}
Result<std::string> object_name(const Json &row, std::string_view field, std::string_view context) {
    auto result = required_text(row, field, context);
    if (!result)
        return std::unexpected{result.error()};
    if (result->size() > 16U || !std::ranges::all_of(*result, [](unsigned char value) { return value < 0x80U; })) {
        return std::unexpected{
            transaction_error(std::string{context} + "." + std::string{field} + " must fit 16 ASCII bytes")};
    }
    return result;
}
Result<std::string> partition_name(const Json &row, std::string_view field, std::string_view context) {
    auto result = required_text(row, field, context);
    if (!result)
        return std::unexpected{result.error()};
    const auto printable =
        std::ranges::all_of(*result, [](unsigned char value) { return value >= 0x20U && value <= 0x7eU; });
    if (result->size() > 16U || !printable || result->front() == ' ' || result->back() == ' ') {
        return std::unexpected{transaction_error(std::string{context} + "." + std::string{field} +
                                                 " must be 1..16 printable ASCII characters without outer spaces")};
    }
    return result;
}
Result<std::string> program_name(const Json &row, std::string_view field, std::string_view context) {
    auto result = required_text(row, field, context);
    if (!result)
        return std::unexpected{result.error()};
    const auto printable =
        std::ranges::all_of(*result, [](unsigned char value) { return value >= 0x20U && value <= 0x7eU; });
    if (result->size() > 8U || !printable || result->front() == ' ' || result->back() == ' ') {
        return std::unexpected{transaction_error(std::string{context} + "." + std::string{field} +
                                                 " must be 1..8 printable ASCII characters without outer spaces")};
    }
    return result;
}

} // namespace

Result<AlterationManifest> parse_alteration_manifest(std::string_view json,
                                                     const std::filesystem::path &base_directory) {
    try {
        auto root = Json::parse(json);
        if (auto valid = exact_fields(root, {"schema_version", "operations"}, "manifest"); !valid)
            return std::unexpected{valid.error()};
        auto version = required_text(root, "schema_version", "manifest");
        if (!version)
            return std::unexpected{version.error()};
        if (*version != alteration_manifest_schema_version)
            return std::unexpected{transaction_error("manifest schema version must be 1.0")};
        if (!root["operations"].is_array() || root["operations"].empty())
            return std::unexpected{transaction_error("manifest.operations must be a non-empty array")};
        AlterationManifest result{*version, {}};
        std::set<std::string> seen;
        for (std::size_t index = 0; index < root["operations"].size(); ++index) {
            const auto &row = root["operations"][index];
            const auto context = "manifest.operations[" + std::to_string(index) + "]";
            if (!row.is_object())
                return std::unexpected{transaction_error(context + " must be an object")};
            auto id = required_text(row, "id", context);
            auto type = required_text(row, "type", context);
            if (!id)
                return std::unexpected{id.error()};
            if (!type)
                return std::unexpected{type.error()};
            if (!seen.insert(*id).second)
                return std::unexpected{transaction_error("duplicate operation id")};
            if (*type != "delete_volume" && *type != "insert_volume" && *type != "delete_sbnk" &&
                *type != "insert_sbnk" && *type != "insert_waveform" && *type != "delete_waveform" &&
                *type != "delete_program" && *type != "insert_program" && *type != "delete_sbac" &&
                *type != "insert_sbac" && *type != "rename_waveform" && *type != "rename_sbnk" &&
                *type != "assign_sbac_members" && *type != "rename_sbac" && *type != "rename_program" &&
                *type != "delete_sequence" && *type != "insert_sequence" && *type != "rename_sequence" &&
                *type != "rename_volume" && *type != "rename_partition" && *type != "repair_object_placements" &&
                *type != "import_tx16w_disk_set" && *type != "clear_program_assignments") {
                return std::unexpected{transaction_error("operation type is not implemented by "
                                                         "the native transaction engine")};
            }
            PartitionSelector selector;
            if (row["partition_index"].is_number_integer()) {
                const auto value = row["partition_index"].get<int>();
                if (value < 0 || value > 7)
                    return std::unexpected{transaction_error("partition index must be 0..7")};
                selector = PartitionIndex{static_cast<std::uint8_t>(value)};
            } else if (row["partition_index"].is_object() && row["partition_index"].size() == 1U &&
                       row["partition_index"].contains("operation_ref")) {
                auto reference = required_text(row["partition_index"], "operation_ref", context + ".partition_index");
                if (!reference)
                    return std::unexpected{reference.error()};
                if (!seen.contains(*reference))
                    return std::unexpected{transaction_error("operation_ref must name an earlier operation")};
                selector = OperationReference{*reference};
            } else
                return std::unexpected{transaction_error("partition selector is invalid")};
            AlterationOperationData data;
            if (*type == "repair_object_placements") {
                auto parsed = detail::parse_placement_operation_json(row, std::move(selector), context);
                if (!parsed)
                    return std::unexpected{parsed.error()};
                data = std::move(*parsed);
            } else if (*type == "import_tx16w_disk_set") {
                if (auto valid = exact_fields(
                        row, {"id", "type", "partition_index", "volume_name", "disk_paths", "import_mode"}, context);
                    !valid)
                    return std::unexpected{valid.error()};
                auto volume = required_text(row, "volume_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                if (!row["disk_paths"].is_array() || row["disk_paths"].empty())
                    return std::unexpected{transaction_error(context + ".disk_paths must not be empty")};
                std::vector<std::filesystem::path> disk_paths;
                disk_paths.reserve(row["disk_paths"].size());
                for (std::size_t path_index = 0U; path_index < row["disk_paths"].size(); ++path_index) {
                    if (!row["disk_paths"][path_index].is_string())
                        return std::unexpected{transaction_error(context + ".disk_paths entries must be strings")};
                    auto disk_path = axk::text::path_from_utf8(row["disk_paths"][path_index].get<std::string>());
                    if (!disk_path)
                        return std::unexpected{transaction_error(context + ".disk_paths entry is not UTF-8")};
                    if (disk_path->is_relative())
                        *disk_path = base_directory / *disk_path;
                    disk_paths.push_back(std::move(*disk_path));
                }
                auto mode = required_text(row, "import_mode", context);
                if (!mode)
                    return std::unexpected{mode.error()};
                axk::tx16w::ImportMode import_mode;
                if (*mode == "hierarchy")
                    import_mode = axk::tx16w::ImportMode::hierarchy;
                else if (*mode == "wave_data_only")
                    import_mode = axk::tx16w::ImportMode::wave_data_only;
                else
                    return std::unexpected{transaction_error(context + ".import_mode is invalid")};
                data = ImportTx16wDiskSetOperation{std::move(selector), std::move(*volume), std::move(disk_paths),
                                                   import_mode};
            } else if (*type == "delete_volume") {
                if (auto valid = exact_fields(row, {"id", "type", "partition_index", "volume_name"}, context); !valid)
                    return std::unexpected{valid.error()};
                auto volume = required_text(row, "volume_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                data = DeleteVolumeOperation{std::move(selector), std::move(*volume)};
            } else if (*type == "rename_volume") {
                if (auto valid =
                        exact_fields(row, {"id", "type", "partition_index", "volume_name", "new_volume_name"}, context);
                    !valid) {
                    return std::unexpected{valid.error()};
                }
                auto volume = object_name(row, "volume_name", context);
                auto new_volume = object_name(row, "new_volume_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                if (!new_volume)
                    return std::unexpected{new_volume.error()};
                if (*volume == *new_volume)
                    return std::unexpected{transaction_error("new_volume_name must differ")};
                data = RenameVolumeOperation{std::move(selector), std::move(*volume), std::move(*new_volume)};
            } else if (*type == "rename_partition") {
                if (auto valid = exact_fields(
                        row, {"id", "type", "partition_index", "partition_name", "new_partition_name"}, context);
                    !valid) {
                    return std::unexpected{valid.error()};
                }
                auto old_name = partition_name(row, "partition_name", context);
                auto new_name = partition_name(row, "new_partition_name", context);
                if (!old_name)
                    return std::unexpected{old_name.error()};
                if (!new_name)
                    return std::unexpected{new_name.error()};
                if (*old_name == *new_name)
                    return std::unexpected{transaction_error("new_partition_name must differ")};
                data = RenamePartitionOperation{std::move(selector), std::move(*old_name), std::move(*new_name)};
            } else if (*type == "insert_volume") {
                if (auto valid = exact_fields(row, {"id", "type", "partition_index", "volume"}, context); !valid)
                    return std::unexpected{valid.error()};
                Json wrapper{
                    {"schema_version", build_manifest_schema_version},
                    {"size_bytes", minimum_hds_size},
                    {"partitions", Json::array({
                                       {{"name", "AXK ALTER"}, {"volumes", Json::array({row["volume"]})}},
                                   })},
                };
                auto parsed = parse_hds_build_manifest(wrapper.dump(), base_directory);
                if (!parsed)
                    return std::unexpected{parsed.error()};
                data = InsertVolumeOperation{std::move(selector), std::move(parsed->partitions[0].volumes[0])};
            } else if (*type == "delete_sbnk") {
                if (auto valid =
                        exact_fields(row, {"id", "type", "partition_index", "volume_name", "sample_name"}, context);
                    !valid) {
                    return std::unexpected{valid.error()};
                }
                auto volume = required_text(row, "volume_name", context);
                auto sample = object_name(row, "sample_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                if (!sample)
                    return std::unexpected{sample.error()};
                data = DeleteSampleOperation{std::move(selector), std::move(*volume), std::move(*sample)};
            } else if (*type == "insert_sbnk") {
                if (auto valid = exact_fields(row, {"id", "type", "partition_index", "volume_name", "sample"}, context);
                    !valid) {
                    return std::unexpected{valid.error()};
                }
                auto volume = required_text(row, "volume_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                const auto &sample = row["sample"];
                if (!sample.is_object()) {
                    return std::unexpected{transaction_error(context + ".sample must be an object")};
                }
                const std::set<std::string> required{"name", "waveform_name", "root_key", "key_low", "key_high"};
                const std::set<std::string> optional{
                    "right_waveform_name", "level",          "fine_tune_cents", "velocity_low", "velocity_high",
                    "expand_detune",       "expand_dephase", "expand_width",    "loop_mode",    "loop_start_frame",
                    "loop_length_frames"};
                for (const auto &field : required) {
                    if (!sample.contains(field)) {
                        return std::unexpected{transaction_error(context + ".sample is missing field " + field)};
                    }
                }
                for (const auto &[field, unused] : sample.items()) {
                    static_cast<void>(unused);
                    if (!required.contains(field) && !optional.contains(field)) {
                        return std::unexpected{transaction_error(context + ".sample has unknown field " + field)};
                    }
                }
                const auto sample_context = context + ".sample";
                auto name = object_name(sample, "name", sample_context);
                auto waveform = object_name(sample, "waveform_name", sample_context);
                auto root_key = midi_value(sample, "root_key", sample_context, 0U, true);
                auto key_low = midi_value(sample, "key_low", sample_context, 0U, true, sampler_original_key_low_limit);
                auto key_high =
                    midi_value(sample, "key_high", sample_context, 0U, true, sampler_original_key_high_limit);
                auto level = midi_value(sample, "level", sample_context, 100U, false);
                auto fine = bounded_integer(sample, "fine_tune_cents", sample_context, -63, 63, 0);
                auto velocity_low = midi_value(sample, "velocity_low", sample_context, 0U, false);
                auto velocity_high = midi_value(sample, "velocity_high", sample_context, 127U, false);
                auto expand_detune = bounded_integer(sample, "expand_detune", sample_context, -7, 7, 0);
                auto expand_dephase = bounded_integer(sample, "expand_dephase", sample_context, -63, 63, 0);
                auto expand_width = bounded_integer(sample, "expand_width", sample_context, -63, 63, 63);
                auto loop_mode = bounded_integer(sample, "loop_mode", sample_context, 0, 5, 4);
                auto loop_start = bounded_integer(sample, "loop_start_frame", sample_context, 0,
                                                  maximum_wave_data_frames_per_channel, 0);
                auto loop_length = bounded_integer(sample, "loop_length_frames", sample_context, 0,
                                                   maximum_wave_data_frames_per_channel, 0);
                if (!name)
                    return std::unexpected{name.error()};
                if (!waveform)
                    return std::unexpected{waveform.error()};
                if (!root_key)
                    return std::unexpected{root_key.error()};
                if (!key_low)
                    return std::unexpected{key_low.error()};
                if (!key_high)
                    return std::unexpected{key_high.error()};
                if (!level)
                    return std::unexpected{level.error()};
                if (!fine)
                    return std::unexpected{fine.error()};
                if (!velocity_low)
                    return std::unexpected{velocity_low.error()};
                if (!velocity_high)
                    return std::unexpected{velocity_high.error()};
                if (!expand_detune)
                    return std::unexpected{expand_detune.error()};
                if (!expand_dephase)
                    return std::unexpected{expand_dephase.error()};
                if (!expand_width)
                    return std::unexpected{expand_width.error()};
                if (!loop_mode)
                    return std::unexpected{loop_mode.error()};
                if (!loop_start)
                    return std::unexpected{loop_start.error()};
                if (!loop_length)
                    return std::unexpected{loop_length.error()};
                if (*key_low > 127U && *key_low != sampler_original_key_low_limit) {
                    return std::unexpected{
                        transaction_error(sample_context + ".key_low is outside its supported range")};
                }
                const auto effective_low = *key_low == sampler_original_key_low_limit ? *root_key : *key_low;
                const auto effective_high = *key_high == sampler_original_key_high_limit ? *root_key : *key_high;
                if (effective_high < effective_low) {
                    return std::unexpected{transaction_error(sample_context + ".key_high must not be below key_low")};
                }
                SampleSpec spec;
                spec.name = std::move(*name);
                spec.waveform_id = std::move(*waveform);
                if (sample.contains("right_waveform_name")) {
                    auto right = object_name(sample, "right_waveform_name", sample_context);
                    if (!right)
                        return std::unexpected{right.error()};
                    spec.right_waveform_id = std::move(*right);
                }
                spec.root_key = *root_key;
                spec.key_low = *key_low;
                spec.key_high = *key_high;
                spec.level = *level;
                spec.fine_tune_cents = static_cast<std::int8_t>(*fine);
                spec.velocity_low = *velocity_low;
                spec.velocity_high = *velocity_high;
                spec.expand_detune = static_cast<std::int8_t>(*expand_detune);
                spec.expand_dephase = static_cast<std::int8_t>(*expand_dephase);
                spec.expand_width = static_cast<std::int8_t>(*expand_width);
                spec.loop_mode = static_cast<AudioSamplerLoopMode>(*loop_mode);
                spec.loop_start_frame = static_cast<std::uint32_t>(*loop_start);
                spec.loop_length_frames = static_cast<std::uint32_t>(*loop_length);
                data = InsertSampleOperation{std::move(selector), std::move(*volume), std::move(spec)};
            } else if (*type == "rename_waveform") {
                if (auto valid = exact_fields(
                        row, {"id", "type", "partition_index", "volume_name", "waveform_name", "new_waveform_name"},
                        context);
                    !valid)
                    return std::unexpected{valid.error()};
                auto volume = required_text(row, "volume_name", context);
                auto old_name = object_name(row, "waveform_name", context);
                auto new_name = object_name(row, "new_waveform_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                if (!old_name)
                    return std::unexpected{old_name.error()};
                if (!new_name)
                    return std::unexpected{new_name.error()};
                if (*old_name == *new_name)
                    return std::unexpected{transaction_error("new_waveform_name must differ")};
                data = RenameWaveformOperation{std::move(selector), std::move(*volume), std::move(*old_name),
                                               std::move(*new_name)};
            } else if (*type == "rename_sbnk") {
                if (auto valid = exact_fields(
                        row, {"id", "type", "partition_index", "volume_name", "sample_name", "new_sample_name"},
                        context);
                    !valid)
                    return std::unexpected{valid.error()};
                auto volume = required_text(row, "volume_name", context);
                auto old_name = object_name(row, "sample_name", context);
                auto new_name = object_name(row, "new_sample_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                if (!old_name)
                    return std::unexpected{old_name.error()};
                if (!new_name)
                    return std::unexpected{new_name.error()};
                if (*old_name == *new_name)
                    return std::unexpected{transaction_error("new_sample_name must differ")};
                data = RenameSampleOperation{std::move(selector), std::move(*volume), std::move(*old_name),
                                             std::move(*new_name)};
            } else if (*type == "rename_sbac") {
                if (auto valid = exact_fields(
                        row,
                        {"id", "type", "partition_index", "volume_name", "sample_bank_name", "new_sample_bank_name"},
                        context);
                    !valid)
                    return std::unexpected{valid.error()};
                auto volume = required_text(row, "volume_name", context);
                auto old_name = object_name(row, "sample_bank_name", context);
                auto new_name = object_name(row, "new_sample_bank_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                if (!old_name)
                    return std::unexpected{old_name.error()};
                if (!new_name)
                    return std::unexpected{new_name.error()};
                if (*old_name == *new_name)
                    return std::unexpected{transaction_error("new_sample_bank_name must differ")};
                data = RenameSampleBankOperation{std::move(selector), std::move(*volume), std::move(*old_name),
                                                 std::move(*new_name)};
            } else if (*type == "delete_program") {
                if (auto valid =
                        exact_fields(row, {"id", "type", "partition_index", "volume_name", "program_number"}, context);
                    !valid)
                    return std::unexpected{valid.error()};
                auto volume = required_text(row, "volume_name", context);
                auto number = program_value(row, "program_number", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                if (!number)
                    return std::unexpected{number.error()};
                data = DeleteProgramOperation{std::move(selector), std::move(*volume), *number};
            } else if (*type == "delete_sbac") {
                if (auto valid = exact_fields(row, {"id", "type", "partition_index", "volume_name", "sample_bank_name"},
                                              context);
                    !valid)
                    return std::unexpected{valid.error()};
                auto volume = required_text(row, "volume_name", context);
                auto sample_bank = object_name(row, "sample_bank_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                if (!sample_bank)
                    return std::unexpected{sample_bank.error()};
                data = DeleteSampleBankOperation{std::move(selector), std::move(*volume), std::move(*sample_bank)};
            } else if (*type == "insert_sbac") {
                auto parsed = detail::parse_insert_sample_bank_json(row, std::move(selector), context);
                if (!parsed)
                    return std::unexpected{parsed.error()};
                data = std::move(*parsed);
            } else if (*type == "assign_sbac_members") {
                auto assignment = detail::parse_sample_bank_assignment_json(row, std::move(selector), context);
                if (!assignment)
                    return std::unexpected{assignment.error()};
                data = std::move(*assignment);
            } else if (*type == "insert_program") {
                auto program = detail::parse_insert_program_json(row, std::move(selector), context);
                if (!program)
                    return std::unexpected{program.error()};
                data = std::move(*program);
            } else if (*type == "rename_program") {
                if (auto valid = exact_fields(
                        row, {"id", "type", "partition_index", "volume_name", "program_number", "new_program_name"},
                        context);
                    !valid) {
                    return std::unexpected{valid.error()};
                }
                auto volume = required_text(row, "volume_name", context);
                auto number = program_value(row, "program_number", context);
                auto name = program_name(row, "new_program_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                if (!number)
                    return std::unexpected{number.error()};
                if (!name)
                    return std::unexpected{name.error()};
                data = RenameProgramOperation{std::move(selector), std::move(*volume), *number, std::move(*name)};
            } else if (*type == "clear_program_assignments") {
                auto cleanup = detail::parse_clear_program_assignments_json(row, std::move(selector), context);
                if (!cleanup)
                    return std::unexpected{cleanup.error()};
                data = std::move(*cleanup);
            } else if (*type == "delete_waveform") {
                if (auto valid =
                        exact_fields(row, {"id", "type", "partition_index", "volume_name", "waveform_name"}, context);
                    !valid) {
                    return std::unexpected{valid.error()};
                }
                auto volume = required_text(row, "volume_name", context);
                auto waveform = object_name(row, "waveform_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                if (!waveform)
                    return std::unexpected{waveform.error()};
                data = DeleteWaveformOperation{std::move(selector), std::move(*volume), std::move(*waveform)};
            } else if (*type == "delete_sequence" || *type == "insert_sequence" || *type == "rename_sequence") {
                auto sequence =
                    detail::parse_sequence_operation_json(row, *type, std::move(selector), base_directory, context);
                if (!sequence)
                    return std::unexpected{sequence.error()};
                data = std::move(*sequence);
            } else {
                if (auto valid = exact_fields(row, {"id", "type", "partition_index", "volume_name", "audio"}, context);
                    !valid) {
                    return std::unexpected{valid.error()};
                }
                auto volume = required_text(row, "volume_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                const auto &audio = row["audio"];
                if (!audio.is_object()) {
                    return std::unexpected{transaction_error(context + ".audio must be an object")};
                }
                const std::set<std::string> required{"path", "waveform_names", "root_key"};
                const std::set<std::string> optional{"target_sample_rate", "fine_tune_cents", "loop_mode",
                                                     "loop_start_frame", "loop_length_frames"};
                for (const auto &field : required) {
                    if (!audio.contains(field)) {
                        return std::unexpected{transaction_error(context + ".audio is missing field " + field)};
                    }
                }
                for (const auto &[field, unused] : audio.items()) {
                    static_cast<void>(unused);
                    if (!required.contains(field) && !optional.contains(field)) {
                        return std::unexpected{transaction_error(context + ".audio has unknown field " + field)};
                    }
                }
                if (!audio["waveform_names"].is_array() ||
                    (audio["waveform_names"].size() != 1U && audio["waveform_names"].size() != 2U)) {
                    return std::unexpected{
                        transaction_error(context + ".audio.waveform_names must contain one or two names")};
                }
                InsertWaveformSpec spec;
                for (std::size_t name_index = 0; name_index < audio["waveform_names"].size(); ++name_index) {
                    Json wrapper{{"name", audio["waveform_names"][name_index]}};
                    auto name = object_name(wrapper, "name",
                                            context + ".audio.waveform_names[" + std::to_string(name_index) + "]");
                    if (!name)
                        return std::unexpected{name.error()};
                    if (std::ranges::contains(spec.waveform_names, *name)) {
                        return std::unexpected{transaction_error(context + ".audio.waveform_names must be distinct")};
                    }
                    spec.waveform_names.push_back(std::move(*name));
                }
                auto path = required_text(audio, "path", context + ".audio");
                auto root_key = midi_value(audio, "root_key", context + ".audio", 0U, true);
                auto fine = bounded_integer(audio, "fine_tune_cents", context + ".audio", -63, 63, 0);
                auto loop_mode = bounded_integer(audio, "loop_mode", context + ".audio", 0, 5, 4);
                auto loop_start = bounded_integer(audio, "loop_start_frame", context + ".audio", 0,
                                                  maximum_wave_data_frames_per_channel, 0);
                auto loop_length = bounded_integer(audio, "loop_length_frames", context + ".audio", 0,
                                                   maximum_wave_data_frames_per_channel, 0);
                if (!path)
                    return std::unexpected{path.error()};
                if (!root_key)
                    return std::unexpected{root_key.error()};
                if (!fine)
                    return std::unexpected{fine.error()};
                if (!loop_mode)
                    return std::unexpected{loop_mode.error()};
                if (!loop_start)
                    return std::unexpected{loop_start.error()};
                if (!loop_length)
                    return std::unexpected{loop_length.error()};
                auto audio_path = axk::text::path_from_utf8(*path);
                if (!audio_path)
                    return std::unexpected{transaction_error(context + ".audio.path must be valid UTF-8")};
                spec.path = std::move(*audio_path);
                if (spec.path.is_relative())
                    spec.path = base_directory / spec.path;
                spec.root_key = *root_key;
                spec.fine_tune_cents = static_cast<std::int8_t>(*fine);
                spec.loop_mode = static_cast<AudioSamplerLoopMode>(*loop_mode);
                spec.loop_start_frame = static_cast<std::uint32_t>(*loop_start);
                spec.loop_length_frames = static_cast<std::uint32_t>(*loop_length);
                if (audio.contains("target_sample_rate")) {
                    if (!audio["target_sample_rate"].is_number_integer()) {
                        return std::unexpected{
                            transaction_error(context + ".audio.target_sample_rate must be an integer")};
                    }
                    const auto rate = audio["target_sample_rate"].get<std::int64_t>();
                    if (rate <= 0 || rate > std::numeric_limits<std::uint32_t>::max()) {
                        return std::unexpected{
                            transaction_error(context + ".audio.target_sample_rate is out of range")};
                    }
                    spec.target_sample_rate = static_cast<std::uint32_t>(rate);
                }
                data = InsertWaveformOperation{std::move(selector), std::move(*volume), std::move(spec)};
            }
            result.operations.push_back({std::move(*id), std::move(data)});
        }
        if (auto valid = detail::validate_alteration_manifest(result); !valid)
            return std::unexpected{valid.error()};
        return result;
    } catch (const Json::exception &error) {
        return std::unexpected{transaction_error(std::string{"invalid alteration JSON: "} + error.what())};
    }
}

Result<AlterationManifest> load_alteration_manifest(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input)
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not open alteration manifest")};
    std::ostringstream text;
    text << input.rdbuf();
    return parse_alteration_manifest(text.str(), path.parent_path());
}

} // namespace axk
