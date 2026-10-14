#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "axklib/application/image_sessions.hpp"
#include "axklib/catalog.hpp"
#include "axklib/media.hpp"
#include "axklib/relationship.hpp"
#include "image_sources.hpp"

namespace axk::app::formats::a_series {
struct CompanionSegments {
    std::vector<ImageSourceRef> sources;
};

Result<CompanionSegments> append_required_companion_wave_data(
    const axk::AxkObjectDirectory &primary, const std::vector<std::shared_ptr<ImageSourceMember>> &companions,
    std::vector<axk::AxkObjectDirectoryEntry> &entries, const CancellationToken &cancellation);
Result<std::vector<DirectoryRef>> immediate_sibling_directories(const Sandbox &sandbox, const ImageSourceRef &source);
std::string object_format_name(axk::ObjectFormat format);
std::string object_type_name(axk::ObjectType type);
std::string relationship_quality_wire_name(axk::RelationshipQuality quality);
std::optional<std::string> mapped_id(const std::unordered_map<std::string, std::string> &ids, const std::string &key);
std::optional<std::uint8_t> partition_index_from_node_id(std::string_view node_id);
std::uint64_t record_cluster_count(const axk::Container &container, const axk::ObjectSnapshot &object);

} // namespace axk::app::formats::a_series
