#include "alteration_manifest_sequence.hpp"

#include <algorithm>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "axklib/utf8.hpp"

namespace axk::detail {
namespace {

Error manifest_error(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
}

Result<void> exact_fields(const nlohmann::json &row, const std::set<std::string> &expected, std::string_view context) {
    if (!row.is_object() || row.size() != expected.size())
        return std::unexpected{manifest_error(std::string{context} + " has invalid fields")};
    for (const auto &field : expected) {
        if (!row.contains(field))
            return std::unexpected{manifest_error(std::string{context} + " is missing field " + field)};
    }
    return {};
}

Result<std::string> object_name(const nlohmann::json &row, std::string_view field, std::string_view context) {
    if (!row.contains(field) || !row[field].is_string() || row[field].get_ref<const std::string &>().empty())
        return std::unexpected{
            manifest_error(std::string{context} + "." + std::string{field} + " must be a non-empty string")};
    auto value = row[field].get<std::string>();
    const auto printable =
        std::ranges::all_of(value, [](unsigned char character) { return character >= 0x20U && character < 0x7fU; });
    if (value.size() > 16U || !printable)
        return std::unexpected{
            manifest_error(std::string{context} + "." + std::string{field} + " must fit 16 printable ASCII bytes")};
    return value;
}

Result<std::string> text_field(const nlohmann::json &row, std::string_view field, std::string_view context) {
    if (!row.contains(field) || !row[field].is_string() || row[field].get_ref<const std::string &>().empty())
        return std::unexpected{
            manifest_error(std::string{context} + "." + std::string{field} + " must be a non-empty string")};
    return row[field].get<std::string>();
}

} // namespace

Result<AlterationOperationData> parse_sequence_operation_json(const nlohmann::json &row, std::string_view type,
                                                              PartitionSelector selector,
                                                              const std::filesystem::path &base_directory,
                                                              std::string_view context) {
    if (type == "delete_sequence") {
        if (auto valid = exact_fields(row, {"id", "partition_index", "sequence_name", "type", "volume_name"}, context);
            !valid)
            return std::unexpected{valid.error()};
        auto volume = text_field(row, "volume_name", context);
        auto name = object_name(row, "sequence_name", context);
        if (!volume)
            return std::unexpected{volume.error()};
        if (!name)
            return std::unexpected{name.error()};
        return DeleteSequenceOperation{std::move(selector), std::move(*volume), std::move(*name)};
    }
    if (type == "rename_sequence") {
        if (auto valid = exact_fields(
                row, {"id", "new_sequence_name", "partition_index", "sequence_name", "type", "volume_name"}, context);
            !valid)
            return std::unexpected{valid.error()};
        auto volume = text_field(row, "volume_name", context);
        auto old_name = object_name(row, "sequence_name", context);
        auto new_name = object_name(row, "new_sequence_name", context);
        if (!volume)
            return std::unexpected{volume.error()};
        if (!old_name)
            return std::unexpected{old_name.error()};
        if (!new_name)
            return std::unexpected{new_name.error()};
        return RenameSequenceOperation{std::move(selector), std::move(*volume), std::move(*old_name),
                                       std::move(*new_name)};
    }
    if (auto valid = exact_fields(row, {"id", "partition_index", "sequence", "type", "volume_name"}, context); !valid)
        return std::unexpected{valid.error()};
    if (!row["sequence"].is_object())
        return std::unexpected{manifest_error(std::string{context} + ".sequence must be an object")};
    if (auto valid = exact_fields(row["sequence"], {"midi_path", "name"}, std::string{context} + ".sequence"); !valid)
        return std::unexpected{valid.error()};
    auto volume = text_field(row, "volume_name", context);
    auto name = object_name(row["sequence"], "name", std::string{context} + ".sequence");
    auto path = text_field(row["sequence"], "midi_path", std::string{context} + ".sequence");
    if (!volume)
        return std::unexpected{volume.error()};
    if (!name)
        return std::unexpected{name.error()};
    if (!path)
        return std::unexpected{path.error()};
    auto native_path = text::path_from_utf8(*path);
    if (!native_path)
        return std::unexpected{manifest_error(std::string{context} + ".sequence.midi_path must be valid UTF-8")};
    if (native_path->is_relative())
        *native_path = base_directory / *native_path;
    return InsertSequenceOperation{
        std::move(selector), std::move(*volume), {std::move(*name), std::move(*native_path)}};
}

} // namespace axk::detail
