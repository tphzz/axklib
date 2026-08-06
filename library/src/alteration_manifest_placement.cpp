#include "alteration_manifest_placement.hpp"

#include <cstdint>
#include <limits>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

namespace axk::detail {
namespace {

Error manifest_error(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
}

} // namespace

Result<AlterationOperationData> parse_placement_operation_json(const nlohmann::json &row, PartitionSelector selector,
                                                               std::string_view context) {
    constexpr std::size_t field_count = 5U;
    if (!row.is_object() || row.size() != field_count || !row.contains("id") || !row.contains("type") ||
        !row.contains("partition_index") || !row.contains("volume_name") || !row.contains("object_sfs_ids")) {
        return std::unexpected{manifest_error(std::string{context} + " has invalid fields")};
    }
    if (!row["volume_name"].is_string() || row["volume_name"].get_ref<const std::string &>().empty()) {
        return std::unexpected{manifest_error(std::string{context} + ".volume_name must be a non-empty string")};
    }
    const auto &ids = row["object_sfs_ids"];
    if (!ids.is_array() || ids.empty() || ids.size() > 4096U) {
        return std::unexpected{manifest_error(std::string{context} + ".object_sfs_ids must contain 1..4096 SFS IDs")};
    }
    std::vector<SfsId> parsed_ids;
    parsed_ids.reserve(ids.size());
    std::set<std::uint32_t> unique;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (!ids[index].is_number_integer()) {
            return std::unexpected{manifest_error(std::string{context} + ".object_sfs_ids must contain integers")};
        }
        const auto value = ids[index].get<std::int64_t>();
        if (value <= 2 || value > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected{
                manifest_error(std::string{context} + ".object_sfs_ids contains a non-object SFS ID")};
        }
        const auto id = static_cast<std::uint32_t>(value);
        if (!unique.insert(id).second) {
            return std::unexpected{
                manifest_error(std::string{context} + ".object_sfs_ids must not contain duplicates")};
        }
        parsed_ids.push_back(SfsId{id});
    }
    return RepairObjectPlacementsOperation{std::move(selector), row["volume_name"].get<std::string>(),
                                           std::move(parsed_ids)};
}

} // namespace axk::detail
