#include "axklib/writer_internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace axk::detail {
namespace {

constexpr std::uint64_t sector_size = 2048U;
constexpr std::uint32_t first_path_table_sector = 18U;

void little16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void big16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>(value & 0xffU);
}

void little32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index)
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
}

void big32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index)
        bytes[offset + index] = static_cast<std::byte>((value >> ((3U - index) * 8U)) & 0xffU);
}

std::string category(ObjectType type) {
    switch (type) {
    case ObjectType::smpl:
        return "SMPL";
    case ObjectType::sbnk:
        return "SBNK";
    case ObjectType::sbac:
        return "SBAC";
    case ObjectType::prog:
        return "PROG";
    case ObjectType::sequ:
        return "SEQU";
    case ObjectType::prf3:
        return "PRF3";
    case ObjectType::unknown:
        return "OTHER";
    }
    return "OTHER";
}

bool iso_identifier(std::string_view value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum && std::ranges::all_of(value, [](unsigned char character) {
        return std::isupper(character) != 0 || std::isdigit(character) != 0 || character == '_';
    });
}

std::vector<std::byte> label_file(std::string_view label) {
    std::vector<std::byte> result(16U, std::byte{' '});
    std::ranges::transform(label.substr(0, result.size()), result.begin(),
                           [](char character) { return static_cast<std::byte>(character); });
    return result;
}

std::uint8_t catalog_hash(std::span<const std::byte> value) {
    constexpr std::array<std::uint8_t, 4> table{0xaaU, 0x55U, 0xc3U, 0x3cU};
    std::uint8_t result{};
    for (const auto byte : value.first(std::min<std::size_t>(value.size(), 16U))) {
        const auto character = std::to_integer<std::uint8_t>(byte);
        if (character == 0U)
            break;
        result = static_cast<std::uint8_t>((result ^ table[result & 3U]) + character);
    }
    return result;
}

std::vector<std::byte> catalog_record(std::string_view display_name, std::string_view filename) {
    std::vector<std::byte> result(32U);
    std::fill_n(result.begin() + 1, 16U, std::byte{' '});
    std::ranges::transform(display_name.substr(0, 16U), result.begin() + 1,
                           [](char character) { return static_cast<std::byte>(character); });
    result[0] = static_cast<std::byte>(catalog_hash(std::span{result}.subspan(1, 16U)));
    std::ranges::transform(filename.substr(0, 11U), result.begin() + 18,
                           [](char character) { return static_cast<std::byte>(character); });
    result[17] = static_cast<std::byte>(catalog_hash(std::as_bytes(std::span{filename})));
    return result;
}

std::vector<std::byte> disk_name_record(std::string_view filename) {
    std::vector<std::byte> result(32U);
    constexpr std::string_view marker{"_DSKNAME"};
    std::ranges::transform(marker, result.begin() + 1,
                           [](char character) { return static_cast<std::byte>(character); });
    result[0] = static_cast<std::byte>(catalog_hash(std::span{result}.subspan(1, 16U)));
    std::ranges::transform(filename.substr(0, 11U), result.begin() + 18,
                           [](char character) { return static_cast<std::byte>(character); });
    result[17] = static_cast<std::byte>(catalog_hash(std::as_bytes(std::span{filename})));
    return result;
}

std::size_t directory_record_size(std::size_t name_size) { return 33U + name_size + (name_size % 2U == 0U ? 1U : 0U); }

std::uint64_t sector_count(std::uint64_t bytes) { return bytes / sector_size + (bytes % sector_size == 0U ? 0U : 1U); }

Result<std::uint32_t> checked_sector_add(std::uint32_t current, std::uint64_t additional) {
    if (additional > std::numeric_limits<std::uint32_t>::max() - current) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "ISO9660 sector count exceeds the 32-bit extent profile")};
    }
    return current + static_cast<std::uint32_t>(additional);
}

Result<std::uint32_t> packed_directory_size(std::size_t index, std::span<const Iso9660LayoutNode> nodes) {
    std::uint64_t offset = directory_record_size(1U) * 2U;
    for (std::size_t child_index = 1U; child_index < nodes.size(); ++child_index) {
        const auto &child = nodes[child_index];
        if (child.parent != index)
            continue;
        const auto record_size = directory_record_size(child.name.size());
        const auto remaining = sector_size - offset % sector_size;
        if (record_size > remaining)
            offset += remaining;
        offset += record_size;
    }
    const auto bytes = sector_count(offset) * sector_size;
    if (bytes > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "ISO9660 directory exceeds the 32-bit extent profile")};
    }
    return static_cast<std::uint32_t>(bytes);
}

void append_path_record(std::vector<std::byte> &table, const Iso9660LayoutNode &node, std::uint16_t parent_number,
                        bool big_endian) {
    const std::string_view name = node.name.empty() ? std::string_view{"\0", 1} : std::string_view{node.name};
    const auto offset = table.size();
    table.resize(offset + 8U + name.size() + (name.size() % 2U));
    table[offset] = static_cast<std::byte>(name.size());
    if (big_endian) {
        big32(table, offset + 2U, node.sector);
        big16(table, offset + 6U, parent_number);
    } else {
        little32(table, offset + 2U, node.sector);
        little16(table, offset + 6U, parent_number);
    }
    std::ranges::transform(name, table.begin() + static_cast<std::ptrdiff_t>(offset + 8U),
                           [](char character) { return static_cast<std::byte>(character); });
}

} // namespace

Result<Iso9660Layout> plan_iso9660_layout(const PreparedMediaImage &image) {
    const auto &manifest = image.manifest;
    if (!iso_identifier(manifest.iso_volume_id, 32U)) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "ISO9660 writer requires an uppercase ISO volume identifier")};
    }

    struct VolumeView {
        std::string_view raw_group;
        std::string_view group_name;
        std::string_view raw_volume;
        std::string_view volume_name;
        std::span<const PreparedMediaObject> objects;
    };
    std::vector<VolumeView> volumes;
    if (image.iso_volumes.empty()) {
        volumes.push_back(
            {manifest.raw_group, manifest.group_name, manifest.raw_volume, manifest.volume_name, image.objects});
    } else {
        volumes.reserve(image.iso_volumes.size());
        for (const auto &volume : image.iso_volumes) {
            volumes.push_back(
                {volume.raw_group, volume.group_name, volume.raw_volume, volume.volume_name, volume.objects});
        }
    }
    std::ranges::sort(volumes, [](const auto &left, const auto &right) {
        return std::tie(left.raw_group, left.raw_volume) < std::tie(right.raw_group, right.raw_volume);
    });

    std::map<std::string, std::vector<const VolumeView *>, std::less<>> groups;
    std::set<std::pair<std::string, std::string>> volume_ids;
    std::map<std::string, std::string, std::less<>> group_labels;
    for (const auto &volume : volumes) {
        if (!iso_identifier(volume.raw_group, 8U) || volume.raw_volume.size() != 4U || volume.raw_volume[0] != 'F' ||
            volume.raw_volume == "F000" ||
            !std::ranges::all_of(std::string_view{volume.raw_volume}.substr(1),
                                 [](unsigned char value) { return std::isdigit(value) != 0; }) ||
            volume.group_name.empty() || volume.group_name.size() > 16U || volume.volume_name.empty() ||
            volume.volume_name.size() > 16U || !volume_ids.emplace(volume.raw_group, volume.raw_volume).second) {
            return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                              "ISO9660 writer requires unique uppercase groups, F001..F999 "
                                              "volumes, and bounded Yamaha menu labels")};
        }
        const auto [label, inserted] = group_labels.emplace(volume.raw_group, volume.group_name);
        if (!inserted && label->second != volume.group_name) {
            return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                              "ISO9660 volumes in one raw group must share one "
                                              "sampler-visible group label")};
        }
        groups[std::string{volume.raw_group}].push_back(&volume);
    }
    for (const auto &[raw_group, group_volumes] : groups) {
        for (std::size_t index = 0; index < group_volumes.size(); ++index) {
            if (group_volumes[index]->raw_volume != std::format("F{:03}", index + 1U)) {
                return std::unexpected{make_error(
                    ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                    std::format("ISO9660 raw group '{}' must use contiguous volumes F001..Fnnn", raw_group))};
            }
        }
    }

    Iso9660Layout layout;
    auto &nodes = layout.nodes;
    nodes.push_back({"", true, 0U, 0U, 0U, {}, nullptr});
    const auto add_directory = [&](std::string name, std::size_t parent) -> Result<std::size_t> {
        if (!iso_identifier(name, 31U)) {
            return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                              "ISO9660 path component is outside the narrow profile")};
        }
        if (std::ranges::find_if(nodes, [&](const auto &node) { return node.parent == parent && node.name == name; }) !=
            nodes.end()) {
            return std::unexpected{
                make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest, "duplicate ISO9660 path")};
        }
        nodes.push_back({std::move(name), true, 0U, 0U, parent, {}, nullptr});
        return nodes.size() - 1U;
    };
    const auto validate_file = [&](std::string_view name, std::size_t parent) -> Result<void> {
        if (!iso_identifier(name, 31U) || std::ranges::find_if(nodes, [&](const auto &node) {
                                              return node.parent == parent && node.name == name;
                                          }) != nodes.end()) {
            return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                              "invalid or duplicate ISO9660 file path")};
        }
        return {};
    };
    const auto add_owned_file = [&](std::string name, std::vector<std::byte> data, std::size_t parent) -> Result<void> {
        if (auto valid = validate_file(name, parent); !valid)
            return valid;
        nodes.push_back({std::move(name), false, 0U, 0U, parent, std::move(data), nullptr});
        return {};
    };
    const auto add_external_file = [&](std::string name, std::shared_ptr<const RandomAccessReader> data,
                                       std::size_t parent) -> Result<void> {
        if (auto valid = validate_file(name, parent); !valid)
            return valid;
        if (data == nullptr) {
            return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                              "ISO9660 file payload reader is missing")};
        }
        nodes.push_back({std::move(name), false, 0U, 0U, parent, {}, std::move(data)});
        return {};
    };
    const auto find_child = [&](std::size_t parent, std::string_view name) -> std::optional<std::size_t> {
        for (std::size_t index = 1U; index < nodes.size(); ++index) {
            if (nodes[index].parent == parent && nodes[index].name == name)
                return index;
        }
        return std::nullopt;
    };
    const auto ensure_directory = [&](std::string_view path) -> Result<std::size_t> {
        std::size_t parent{};
        std::size_t begin{};
        while (begin < path.size()) {
            const auto end = path.find('/', begin);
            const auto component =
                path.substr(begin, end == std::string_view::npos ? path.size() - begin : end - begin);
            if (component.empty()) {
                return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                                  "ISO9660 retained path contains an empty component")};
            }
            if (const auto child = find_child(parent, component); child) {
                if (!nodes[*child].directory) {
                    return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                                      "ISO9660 retained path collides with a file")};
                }
                parent = *child;
            } else {
                auto created = add_directory(std::string{component}, parent);
                if (!created)
                    return std::unexpected{created.error()};
                parent = *created;
            }
            if (end == std::string_view::npos)
                break;
            begin = end + 1U;
        }
        return parent;
    };

    for (const auto &[raw_group, group_volumes] : groups) {
        auto group = add_directory(raw_group, 0U);
        if (!group)
            return std::unexpected{group.error()};
        std::vector<std::byte> group_catalog;
        for (const auto *volume : group_volumes) {
            const auto record = catalog_record(volume->volume_name, volume->raw_volume);
            group_catalog.insert(group_catalog.end(), record.begin(), record.end());
        }
        const auto group_label_filename = std::format("F{:03}", group_volumes.size() + 1U);
        const auto name_record = disk_name_record(group_label_filename);
        group_catalog.insert(group_catalog.end(), name_record.begin(), name_record.end());
        if (auto added = add_owned_file("0000", std::move(group_catalog), *group); !added)
            return std::unexpected{added.error()};
        std::vector<std::pair<const VolumeView *, std::size_t>> volume_nodes;
        volume_nodes.reserve(group_volumes.size());
        for (const auto *volume : group_volumes) {
            auto volume_node = add_directory(std::string{volume->raw_volume}, *group);
            if (!volume_node)
                return std::unexpected{volume_node.error()};
            volume_nodes.emplace_back(volume, *volume_node);
        }
        if (auto added = add_owned_file(group_label_filename, label_file(group_labels.at(raw_group)), *group); !added)
            return std::unexpected{added.error()};
        for (const auto &[volume, volume_node] : volume_nodes) {
            std::map<std::string, std::vector<const PreparedMediaObject *>> categories;
            for (const auto &object : volume->objects)
                categories[category(object.type)].push_back(&object);
            for (auto &[name, objects] : categories) {
                if (objects.size() > 999U) {
                    return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                                      "ISO9660 Yamaha category contains more than 999 objects")};
                }
                auto category_node = add_directory(name, volume_node);
                if (!category_node)
                    return std::unexpected{category_node.error()};
                std::vector<std::byte> catalog_bytes;
                catalog_bytes.reserve(objects.size() * 32U);
                for (std::size_t index = 0; index < objects.size(); ++index) {
                    const auto filename = std::format("F{:03}", index + 1U);
                    const auto record = catalog_record(objects[index]->name, filename);
                    catalog_bytes.insert(catalog_bytes.end(), record.begin(), record.end());
                }
                if (auto added = add_owned_file("0000", std::move(catalog_bytes), *category_node); !added)
                    return std::unexpected{added.error()};
                for (std::size_t index = 0; index < objects.size(); ++index) {
                    if (auto added = add_external_file(std::format("F{:03}", index + 1U), objects[index]->payload,
                                                       *category_node);
                        !added) {
                        return std::unexpected{added.error()};
                    }
                }
            }
        }
    }

    for (const auto &retained : image.retained_files) {
        const auto separator = retained.path.rfind('/');
        const auto parent_path =
            separator == std::string::npos ? std::string_view{} : std::string_view{retained.path}.substr(0, separator);
        const auto name = separator == std::string::npos ? std::string_view{retained.path}
                                                         : std::string_view{retained.path}.substr(separator + 1U);
        auto parent = ensure_directory(parent_path);
        if (!parent)
            return std::unexpected{parent.error()};
        if (auto added =
                add_external_file(std::string{name}, std::make_shared<MemoryReader>(retained.payload), *parent);
            !added) {
            return std::unexpected{added.error()};
        }
    }

    for (std::size_t index = 0; index < nodes.size(); ++index) {
        if (!nodes[index].directory)
            continue;
        layout.directory_indices.push_back(index);
        auto size = packed_directory_size(index, nodes);
        if (!size)
            return std::unexpected{size.error()};
        nodes[index].extent_size = *size;
    }
    if (layout.directory_indices.size() > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "ISO9660 path table exceeds the 16-bit parent profile")};
    }

    std::uint64_t path_table_bytes{};
    for (const auto index : layout.directory_indices) {
        const auto name_size = nodes[index].name.empty() ? 1U : nodes[index].name.size();
        path_table_bytes += 8U + name_size + name_size % 2U;
    }
    if (path_table_bytes > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "ISO9660 path table exceeds the 32-bit size profile")};
    }
    layout.little_path_sector = first_path_table_sector;
    auto next_sector = checked_sector_add(layout.little_path_sector, sector_count(path_table_bytes));
    if (!next_sector)
        return std::unexpected{next_sector.error()};
    layout.big_path_sector = *next_sector;
    next_sector = checked_sector_add(layout.big_path_sector, sector_count(path_table_bytes));
    if (!next_sector)
        return std::unexpected{next_sector.error()};
    for (const auto index : layout.directory_indices) {
        nodes[index].sector = *next_sector;
        next_sector = checked_sector_add(*next_sector, sector_count(nodes[index].extent_size));
        if (!next_sector)
            return std::unexpected{next_sector.error()};
    }
    for (auto &node : nodes) {
        if (node.directory)
            continue;
        node.sector = *next_sector;
        if (node.payload_size() > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                              "ISO9660 file exceeds the 32-bit extent-size profile")};
        }
        node.extent_size = static_cast<std::uint32_t>(node.payload_size());
        next_sector = checked_sector_add(*next_sector, sector_count(node.payload_size()));
        if (!next_sector)
            return std::unexpected{next_sector.error()};
    }
    layout.sector_count = *next_sector;
    layout.output_bytes = static_cast<std::uint64_t>(layout.sector_count) * sector_size;

    std::map<std::size_t, std::uint16_t> path_numbers;
    for (std::size_t index = 0; index < layout.directory_indices.size(); ++index)
        path_numbers.emplace(layout.directory_indices[index], static_cast<std::uint16_t>(index + 1U));
    for (const auto index : layout.directory_indices) {
        const auto parent_number = index == 0U ? std::uint16_t{1} : path_numbers.at(nodes[index].parent);
        append_path_record(layout.little_path_table, nodes[index], parent_number, false);
        append_path_record(layout.big_path_table, nodes[index], parent_number, true);
    }
    return layout;
}

} // namespace axk::detail
