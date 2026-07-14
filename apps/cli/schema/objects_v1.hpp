#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/object.hpp"

namespace axk::cli::schema::objects_v1 {

inline constexpr std::string_view schema_version{"1.0"};

enum class ContainerShape : std::uint8_t { media, sfs };

struct ObjectOutput {
    std::optional<std::uint8_t> partition_index;
    std::optional<std::uint32_t> sfs_id;
    std::string key;
    std::string logical_path;
    std::string scope_key;
    std::string raw_group;
    std::string raw_volume;
    std::string group_label;
    std::string group_label_status;
    std::string group_label_basis;
    std::string volume_label;
    std::string volume_label_status;
    std::string volume_label_basis;
    std::uint64_t data_offset{};
    std::uint64_t size{};
    std::string structured_path_utf8;
    ObjectHeader header;
    DecodedObject decoded;
};

struct ObjectsOutput {
    ContainerShape shape{ContainerShape::sfs};
    std::string container_kind;
    std::vector<ObjectOutput> objects;
};

Result<std::string> serialize(const ObjectsOutput &output, bool pretty);

} // namespace axk::cli::schema::objects_v1
