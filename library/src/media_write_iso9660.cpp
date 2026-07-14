#include "writer_internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace axk::detail {
namespace {

constexpr std::size_t sector_size = 2048;
constexpr std::string_view yamaha_iso_system_id = "APPLE COMPUTER, INC., TYPE: 0002";

void little16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void big16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>(value & 0xffU);
}

void both16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
    little16(bytes, offset, value);
    big16(bytes, offset + 2U, value);
}

void little32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index)
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
}

void big32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index)
        bytes[offset + index] = static_cast<std::byte>((value >> ((3U - index) * 8U)) & 0xffU);
}

void both32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    little32(bytes, offset, value);
    big32(bytes, offset + 4U, value);
}

void ascii(std::span<std::byte> bytes, std::size_t offset, std::size_t width, std::string_view value) {
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), width, std::byte{' '});
    std::ranges::transform(value.substr(0, width), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                           [](char character) { return static_cast<std::byte>(character); });
}

std::array<std::byte, 7> recording_time() {
    return {std::byte{70}, std::byte{1}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
}

struct IsoNode {
    std::string name;
    bool directory{};
    std::uint32_t sector{};
    std::vector<std::byte> data;
    std::size_t parent{};
};

std::vector<std::byte> directory_record(const IsoNode &node, std::span<const std::byte> name) {
    const auto length = 33U + name.size() + (name.size() % 2U == 0U ? 1U : 0U);
    std::vector<std::byte> result(length);
    result[0] = static_cast<std::byte>(length);
    little32(result, 2, node.sector);
    big32(result, 6, node.sector);
    const auto data_size = node.directory ? sector_size : node.data.size();
    little32(result, 10, static_cast<std::uint32_t>(data_size));
    big32(result, 14, static_cast<std::uint32_t>(data_size));
    const auto time = recording_time();
    std::ranges::copy(time, result.begin() + 18);
    result[25] = node.directory ? std::byte{2} : std::byte{0};
    both16(result, 28, 1);
    result[32] = static_cast<std::byte>(name.size());
    std::ranges::copy(name, result.begin() + 33);
    return result;
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

void append_path_record(std::vector<std::byte> &table, const IsoNode &node, std::uint16_t parent_number,
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

Result<std::uint32_t> checked_iso9660_sector_count(std::size_t directory_count,
                                                   std::span<const std::uint64_t> file_sizes) {
    constexpr std::uint64_t initial_sector_count = 20U;
    constexpr std::uint64_t maximum_sector_count = std::numeric_limits<std::uint32_t>::max();
    if (directory_count > maximum_sector_count - initial_sector_count) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "ISO9660 sector count exceeds the 32-bit extent profile")};
    }
    auto sectors = initial_sector_count + directory_count;
    for (const auto size : file_sizes) {
        const auto whole = size / sector_size;
        const auto remainder = size % sector_size == 0U ? 0U : 1U;
        if (whole > maximum_sector_count - remainder || whole + remainder > maximum_sector_count - sectors) {
            return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                              "ISO9660 sector count exceeds the 32-bit extent profile")};
        }
        sectors += whole + remainder;
    }
    return static_cast<std::uint32_t>(sectors);
}

Result<void> write_iso9660_image(const PreparedMediaImage &image, const std::filesystem::path &temporary_path,
                                 const CancellationToken &cancellation) {
    const auto &manifest = image.manifest;
    if (!iso_identifier(manifest.iso_volume_id, 32U)) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "ISO9660 writer requires an uppercase ISO volume identifier")};
    }

    std::vector<PreparedIsoVolume> volumes = image.iso_volumes;
    if (volumes.empty()) {
        volumes.push_back(
            {manifest.raw_group, manifest.group_name, manifest.raw_volume, manifest.volume_name, image.objects});
    }
    if (volumes.empty())
        return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                          "ISO9660 writer requires at least one Yamaha volume")};
    std::ranges::sort(volumes, [](const auto &left, const auto &right) {
        return std::tie(left.raw_group, left.raw_volume) < std::tie(right.raw_group, right.raw_volume);
    });

    std::map<std::string, std::vector<const PreparedIsoVolume *>, std::less<>> groups;
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
                                              "volumes, and "
                                              "bounded Yamaha menu labels")};
        }
        const auto [label, inserted] = group_labels.emplace(volume.raw_group, volume.group_name);
        if (!inserted && label->second != volume.group_name) {
            return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                              "ISO9660 volumes in one raw group must share one "
                                              "sampler-visible group label")};
        }
        groups[volume.raw_group].push_back(&volume);
    }
    for (const auto &[raw_group, group_volumes] : groups) {
        for (std::size_t index = 0; index < group_volumes.size(); ++index) {
            if (group_volumes[index]->raw_volume != std::format("F{:03}", index + 1U)) {
                return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                                  std::format("ISO9660 raw group '{}' must use contiguous "
                                                              "volumes F001..Fnnn",
                                                              raw_group))};
            }
        }
    }

    std::vector<IsoNode> nodes{{"", true, 0, {}, 0}};
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
        nodes.push_back({std::move(name), true, 0, {}, parent});
        return nodes.size() - 1U;
    };
    const auto add_file = [&](std::string name, std::vector<std::byte> data, std::size_t parent) -> Result<void> {
        if (!iso_identifier(name, 31U) || std::ranges::find_if(nodes, [&](const auto &node) {
                                              return node.parent == parent && node.name == name;
                                          }) != nodes.end()) {
            return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                              "invalid or duplicate ISO9660 file path")};
        }
        nodes.push_back({std::move(name), false, 0, std::move(data), parent});
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
        if (auto added = add_file("0000", std::move(group_catalog), *group); !added)
            return std::unexpected{added.error()};
        std::vector<std::pair<const PreparedIsoVolume *, std::size_t>> volume_nodes;
        volume_nodes.reserve(group_volumes.size());
        for (const auto *volume : group_volumes) {
            auto volume_node = add_directory(volume->raw_volume, *group);
            if (!volume_node)
                return std::unexpected{volume_node.error()};
            volume_nodes.emplace_back(volume, *volume_node);
        }
        if (auto added = add_file(group_label_filename, label_file(group_labels.at(raw_group)), *group); !added) {
            return std::unexpected{added.error()};
        }
        for (const auto &[volume, volume_node] : volume_nodes) {
            std::map<std::string, std::vector<const PreparedMediaObject *>> categories;
            for (const auto &object : volume->objects)
                categories[category(object.type)].push_back(&object);
            for (auto &[name, objects] : categories) {
                std::ranges::sort(objects, [](const auto *left, const auto *right) {
                    return std::tuple{left->name, left->payload.size()} <
                           std::tuple{right->name, right->payload.size()};
                });
                auto category_node = add_directory(name, volume_node);
                if (!category_node)
                    return std::unexpected{category_node.error()};
                std::vector<std::byte> catalog_bytes;
                std::vector<std::string> filenames;
                catalog_bytes.reserve(objects.size() * 32U);
                filenames.reserve(objects.size());
                for (std::size_t index = 0; index < objects.size(); ++index) {
                    auto filename = std::format("F{:03}", index + 1U);
                    const auto record = catalog_record(objects[index]->name, filename);
                    catalog_bytes.insert(catalog_bytes.end(), record.begin(), record.end());
                    filenames.push_back(std::move(filename));
                }
                if (auto added = add_file("0000", std::move(catalog_bytes), *category_node); !added)
                    return std::unexpected{added.error()};
                for (std::size_t index = 0; index < objects.size(); ++index) {
                    if (auto added = add_file(std::move(filenames[index]), objects[index]->payload, *category_node);
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
        if (auto added = add_file(std::string{name}, retained.payload, *parent); !added)
            return std::unexpected{added.error()};
    }

    std::vector<std::uint64_t> file_sizes;
    file_sizes.reserve(nodes.size());
    for (const auto &node : nodes) {
        if (!node.directory)
            file_sizes.push_back(node.data.size());
    }
    const auto directory_count = static_cast<std::size_t>(std::ranges::count(nodes, true, &IsoNode::directory));
    const auto sector_count = checked_iso9660_sector_count(directory_count, file_sizes);
    if (!sector_count)
        return std::unexpected{sector_count.error()};

    constexpr std::uint32_t little_path_sector = 18;
    constexpr std::uint32_t big_path_sector = 19;
    std::uint32_t next_sector = 20;
    for (auto &node : nodes) {
        if (node.directory)
            node.sector = next_sector++;
    }
    for (auto &node : nodes) {
        if (node.directory)
            continue;
        node.sector = next_sector;
        next_sector += static_cast<std::uint32_t>(node.data.size() / sector_size +
                                                  (node.data.size() % sector_size == 0U ? 0U : 1U));
    }
    if (next_sector != *sector_count) {
        return std::unexpected{make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction,
                                          "ISO9660 sector projection disagrees with allocation")};
    }
    std::vector<std::byte> image_bytes(static_cast<std::size_t>(*sector_count) * sector_size);

    std::vector<std::size_t> directory_indices;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        if (nodes[index].directory)
            directory_indices.push_back(index);
    }
    std::map<std::size_t, std::uint16_t> path_numbers;
    for (std::size_t index = 0; index < directory_indices.size(); ++index)
        path_numbers.emplace(directory_indices[index], static_cast<std::uint16_t>(index + 1U));
    std::vector<std::byte> little_path;
    std::vector<std::byte> big_path;
    for (const auto index : directory_indices) {
        const auto parent_number = index == 0 ? std::uint16_t{1} : path_numbers.at(nodes[index].parent);
        append_path_record(little_path, nodes[index], parent_number, false);
        append_path_record(big_path, nodes[index], parent_number, true);
    }
    if (little_path.size() > sector_size || big_path.size() > sector_size) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "ISO9660 path table exceeds the narrow one-sector profile")};
    }
    std::ranges::copy(little_path, image_bytes.begin() + little_path_sector * sector_size);
    std::ranges::copy(big_path, image_bytes.begin() + big_path_sector * sector_size);

    for (const auto index : directory_indices) {
        const auto &node = nodes[index];
        const auto directory_offset = static_cast<std::size_t>(node.sector) * sector_size;
        std::size_t offset{};
        const std::array<std::byte, 1> dot{std::byte{0}};
        const std::array<std::byte, 1> dotdot{std::byte{1}};
        const auto self = directory_record(node, dot);
        const auto parent = directory_record(nodes[index == 0 ? 0 : node.parent], dotdot);
        std::ranges::copy(self, image_bytes.begin() + static_cast<std::ptrdiff_t>(directory_offset));
        offset += self.size();
        std::ranges::copy(parent, image_bytes.begin() + static_cast<std::ptrdiff_t>(directory_offset + offset));
        offset += parent.size();
        for (const auto &child : nodes) {
            if (&child == &nodes[0] || child.parent != index)
                continue;
            const auto name = std::as_bytes(std::span{child.name});
            const auto record = directory_record(child, name);
            if (offset + record.size() > sector_size) {
                return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                                  "ISO9660 directory exceeds the narrow one-sector profile")};
            }
            std::ranges::copy(record, image_bytes.begin() + static_cast<std::ptrdiff_t>(directory_offset + offset));
            offset += record.size();
        }
    }
    for (const auto &node : nodes) {
        if (!node.directory) {
            std::ranges::copy(node.data, image_bytes.begin() + static_cast<std::ptrdiff_t>(node.sector * sector_size));
        }
    }

    auto pvd = std::span{image_bytes}.subspan(16U * sector_size, sector_size);
    pvd[0] = std::byte{1};
    ascii(pvd, 1, 5, "CD001");
    pvd[6] = std::byte{1};
    ascii(pvd, 8, 32, yamaha_iso_system_id);
    ascii(pvd, 40, 32, manifest.iso_volume_id);
    both32(pvd, 80, next_sector);
    both16(pvd, 120, 1);
    both16(pvd, 124, 1);
    both16(pvd, 128, sector_size);
    both32(pvd, 132, static_cast<std::uint32_t>(little_path.size()));
    little32(pvd, 140, little_path_sector);
    big32(pvd, 148, big_path_sector);
    const auto root_record = directory_record(nodes[0], std::array<std::byte, 1>{std::byte{0}});
    std::ranges::copy(root_record, pvd.begin() + 156);
    ascii(pvd, 190, 128, manifest.iso_volume_id);
    ascii(pvd, 318, 128, "AXKLIB");
    ascii(pvd, 446, 128, "AXKLIB");
    ascii(pvd, 574, 128, "AXKLIB");
    for (const auto offset : {813U, 830U, 864U}) {
        ascii(pvd, offset, 16, "1970010100000000");
        pvd[offset + 16U] = std::byte{0};
    }
    pvd[881] = std::byte{1};
    auto terminator = std::span{image_bytes}.subspan(17U * sector_size, sector_size);
    terminator[0] = std::byte{255};
    ascii(terminator, 1, 5, "CD001");
    terminator[6] = std::byte{1};

    if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
    std::ofstream output{temporary_path, std::ios::binary | std::ios::trunc};
    if (!output)
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not create temporary ISO9660 image")};
    output.write(reinterpret_cast<const char *>(image_bytes.data()), static_cast<std::streamsize>(image_bytes.size()));
    if (!output)
        return std::unexpected{
            make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write temporary ISO9660 image")};
    return {};
}

} // namespace axk::detail
