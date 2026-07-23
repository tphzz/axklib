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

#include "axklib/file_publication.hpp"

#include "alteration_manifest_internal.hpp"

namespace axk {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

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
                                std::uint8_t default_value, bool required) {
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
    if (value < 0 || value > 127) {
        return std::unexpected{
            transaction_error(std::string{context} + "." + std::string{field} + " must be between 0 and 127")};
    }
    return static_cast<std::uint8_t>(value);
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

} // namespace

std::string_view operation_type_name(const AlterationOperationData &operation) noexcept {
    constexpr std::array names{
        std::string_view{"delete_volume"},   std::string_view{"insert_volume"},   std::string_view{"delete_sbnk"},
        std::string_view{"insert_sbnk"},     std::string_view{"insert_waveform"}, std::string_view{"delete_waveform"},
        std::string_view{"rename_waveform"}, std::string_view{"rename_sbnk"},     std::string_view{"delete_sbac"},
        std::string_view{"insert_sbac"},     std::string_view{"rename_sbac"},     std::string_view{"delete_program"},
        std::string_view{"insert_program"},  std::string_view{"rename_volume"},   std::string_view{"rename_partition"},
    };
    return names[operation.index()];
}

Result<std::string> serialize_alteration_manifest_template() {
    try {
        OrderedJson operation = OrderedJson::object();
        operation["id"] = "rename-waveform";
        operation["type"] = "rename_waveform";
        operation["partition_index"] = 0;
        operation["volume_name"] = "Volume";
        operation["waveform_name"] = "Old Wave";
        operation["new_waveform_name"] = "New Wave";

        OrderedJson manifest = OrderedJson::object();
        manifest["schema_version"] = alteration_manifest_schema_version;
        manifest["operations"] = OrderedJson::array({std::move(operation)});
        return manifest.dump(2) + "\n";
    } catch (const OrderedJson::exception &error) {
        return std::unexpected{
            transaction_error(std::string{"could not serialize alteration manifest template: "} + error.what())};
    }
}

Result<void> write_alteration_manifest_template(const std::filesystem::path &output_path, bool overwrite) {
    auto serialized = serialize_alteration_manifest_template();
    if (!serialized)
        return std::unexpected{serialized.error()};

    std::error_code filesystem_error;
    if (!overwrite && std::filesystem::exists(output_path, filesystem_error)) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                       "refusing to replace existing alteration manifest: " + text::path_to_utf8(output_path))};
    }
    if (filesystem_error) {
        return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                          "could not inspect alteration manifest output path")};
    }
    if (!output_path.parent_path().empty())
        std::filesystem::create_directories(output_path.parent_path(), filesystem_error);
    if (filesystem_error) {
        return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                          "could not create alteration manifest output directory")};
    }
    auto temporary = detail::write_temporary_file(output_path, [&](const detail::TemporaryFileSink &sink) {
        return sink(std::as_bytes(std::span{serialized->data(), serialized->size()}));
    });
    if (!temporary)
        return std::unexpected{temporary.error()};
    if (const auto published = detail::publish_temporary_file(*temporary, output_path, overwrite); !published) {
        std::filesystem::remove(*temporary, filesystem_error);
        return std::unexpected{published.error()};
    }
    return {};
}

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
                *type != "rename_sbac" && *type != "rename_volume" && *type != "rename_partition") {
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
            if (*type == "delete_volume") {
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
                const std::set<std::string> optional{"right_waveform_name", "level"};
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
                auto key_low = midi_value(sample, "key_low", sample_context, 0U, true);
                auto key_high = midi_value(sample, "key_high", sample_context, 0U, true);
                auto level = midi_value(sample, "level", sample_context, 100U, false);
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
                if (*key_high < *key_low) {
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
                if (auto valid =
                        exact_fields(row, {"id", "type", "partition_index", "volume_name", "sample_bank"}, context);
                    !valid)
                    return std::unexpected{valid.error()};
                auto volume = required_text(row, "volume_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                const auto &sample_bank = row["sample_bank"];
                if (auto valid = exact_fields(sample_bank, {"name", "member_samples"}, context + ".sample_bank");
                    !valid)
                    return std::unexpected{valid.error()};
                auto name = object_name(sample_bank, "name", context + ".sample_bank");
                if (!name)
                    return std::unexpected{name.error()};
                if (!sample_bank["member_samples"].is_array() || sample_bank["member_samples"].empty() ||
                    sample_bank["member_samples"].size() > 3U)
                    return std::unexpected{transaction_error("member_samples must contain 1..3 names")};
                SampleBankSpec spec;
                spec.name = *name;
                for (std::size_t member_index = 0; member_index < sample_bank["member_samples"].size();
                     ++member_index) {
                    Json wrapper{{"name", sample_bank["member_samples"][member_index]}};
                    auto member = object_name(wrapper, "name", context + ".sample_bank.member_samples");
                    if (!member)
                        return std::unexpected{member.error()};
                    if (std::ranges::contains(spec.member_samples, *member))
                        return std::unexpected{transaction_error("member_samples must be distinct")};
                    spec.member_samples.push_back(std::move(*member));
                }
                data = InsertSampleBankOperation{std::move(selector), std::move(*volume), std::move(spec)};
            } else if (*type == "insert_program") {
                if (auto valid =
                        exact_fields(row, {"id", "type", "partition_index", "volume_name", "program"}, context);
                    !valid)
                    return std::unexpected{valid.error()};
                auto volume = required_text(row, "volume_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                const auto &program = row["program"];
                if (auto valid = exact_fields(program, {"number", "assignments"}, context + ".program"); !valid)
                    return std::unexpected{valid.error()};
                auto number = program_value(program, "number", context + ".program");
                if (!number)
                    return std::unexpected{number.error()};
                if (!program["assignments"].is_array() || program["assignments"].size() != 2U)
                    return std::unexpected{transaction_error("Program requires exactly two assignments")};
                ProgramSpec spec;
                spec.number = *number;
                constexpr std::array<std::string_view, 2> target_fields{"sample_bank", "sample"};
                for (std::size_t assignment_index = 0; assignment_index < 2U; ++assignment_index) {
                    const auto &assignment = program["assignments"][assignment_index];
                    const auto field = target_fields[assignment_index];
                    if (auto valid =
                            exact_fields(assignment, {field, "receive_channel"}, context + ".program.assignments");
                        !valid)
                        return std::unexpected{valid.error()};
                    auto target = object_name(assignment, field, context + ".program.assignments");
                    if (!target)
                        return std::unexpected{target.error()};
                    auto channel =
                        midi_value(assignment, "receive_channel", context + ".program.assignments", 0U, true);
                    const auto expected_channel = static_cast<std::uint8_t>(assignment_index + 1U);
                    if (!channel || *channel != expected_channel)
                        return std::unexpected{transaction_error("Program assignments must be SBAC/channel 1 then "
                                                                 "SBNK/channel 2")};
                    spec.assignments.push_back(
                        {assignment_index == 0U ? "SBAC" : "SBNK", std::move(*target), *channel});
                }
                data = InsertProgramOperation{std::move(selector), std::move(*volume), std::move(spec)};
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
                const std::set<std::string> optional{"target_sample_rate"};
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
                if (!path)
                    return std::unexpected{path.error()};
                if (!root_key)
                    return std::unexpected{root_key.error()};
                auto audio_path = axk::text::path_from_utf8(*path);
                if (!audio_path)
                    return std::unexpected{transaction_error(context + ".audio.path must be valid UTF-8")};
                spec.path = std::move(*audio_path);
                if (spec.path.is_relative())
                    spec.path = base_directory / spec.path;
                spec.root_key = *root_key;
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
