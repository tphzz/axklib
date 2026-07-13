#include "writer_internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <map>
#include <set>

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

void ascii(std::span<std::byte> bytes, std::size_t offset, std::size_t width,
           std::string_view value) {
  std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), width, std::byte{' '});
  std::ranges::transform(value.substr(0, width),
                         bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                         [](char character) { return static_cast<std::byte>(character); });
}

std::array<std::byte, 7> recording_time() {
  return {std::byte{70}, std::byte{1}, std::byte{1}, std::byte{0},
          std::byte{0},  std::byte{0}, std::byte{0}};
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
  return !value.empty() && value.size() <= maximum &&
         std::ranges::all_of(value, [](unsigned char character) {
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

void append_path_record(std::vector<std::byte> &table, const IsoNode &node,
                        std::uint16_t parent_number, bool big_endian) {
  const std::string_view name =
      node.name.empty() ? std::string_view{"\0", 1} : std::string_view{node.name};
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

Result<void> write_iso9660_image(const PreparedMediaImage &image,
                                 const std::filesystem::path &temporary_path,
                                 const CancellationToken &cancellation) {
  const auto &manifest = image.manifest;
  if (!iso_identifier(manifest.raw_group, 8U) || manifest.raw_volume.size() != 4U ||
      manifest.raw_volume[0] != 'F' ||
      !std::ranges::all_of(std::string_view{manifest.raw_volume}.substr(1),
                           [](unsigned char value) { return std::isdigit(value) != 0; }) ||
      manifest.raw_volume == "F000" || !iso_identifier(manifest.iso_volume_id, 32U) ||
      manifest.group_name.empty() || manifest.group_name.size() > 16U ||
      manifest.volume_name.empty() || manifest.volume_name.size() > 16U) {
    return std::unexpected{
        make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                   "ISO9660 writer requires uppercase ISO identifiers, an F001..F999 volume, "
                   "and bounded Yamaha menu labels")};
  }

  std::vector<IsoNode> nodes{{"", true, 0, {}, 0}};
  const auto add_directory = [&](std::string name, std::size_t parent) {
    nodes.push_back({std::move(name), true, 0, {}, parent});
    return nodes.size() - 1U;
  };
  const auto add_file = [&](std::string name, std::vector<std::byte> data, std::size_t parent) {
    nodes.push_back({std::move(name), false, 0, std::move(data), parent});
  };
  const auto group = add_directory(manifest.raw_group, 0);
  const auto group_label_number =
      static_cast<unsigned int>(std::stoul(manifest.raw_volume.substr(1))) + 1U;
  if (group_label_number > 999U) {
    return std::unexpected{
        make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                   "ISO9660 writer cannot place a group label after volume F999")};
  }
  const auto group_label_filename = "F" + std::to_string(1000U + group_label_number).substr(1);
  auto group_catalog = catalog_record(manifest.volume_name, manifest.raw_volume);
  const auto name_record = disk_name_record(group_label_filename);
  group_catalog.insert(group_catalog.end(), name_record.begin(), name_record.end());
  add_file("0000", std::move(group_catalog), group);
  const auto volume = add_directory(manifest.raw_volume, group);
  add_file(group_label_filename, label_file(manifest.group_name), group);

  std::map<std::string, std::vector<const PreparedMediaObject *>> categories;
  for (const auto &object : image.objects)
    categories[category(object.type)].push_back(&object);
  for (const auto &[name, objects] : categories) {
    const auto category_node = add_directory(name, volume);
    std::vector<std::byte> catalog_bytes;
    std::vector<std::string> filenames;
    catalog_bytes.reserve(objects.size() * 32U);
    filenames.reserve(objects.size());
    for (std::size_t index = 0; index < objects.size(); ++index) {
      auto filename = "F" + std::to_string(1001U + static_cast<unsigned int>(index)).substr(1);
      const auto record = catalog_record(objects[index]->name, filename);
      catalog_bytes.insert(catalog_bytes.end(), record.begin(), record.end());
      filenames.push_back(std::move(filename));
    }
    add_file("0000", std::move(catalog_bytes), category_node);
    for (std::size_t index = 0; index < objects.size(); ++index)
      add_file(std::move(filenames[index]), objects[index]->payload, category_node);
  }

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
    next_sector += static_cast<std::uint32_t>((node.data.size() + sector_size - 1U) / sector_size);
  }
  std::vector<std::byte> image_bytes(static_cast<std::size_t>(next_sector) * sector_size);

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
    std::ranges::copy(parent,
                      image_bytes.begin() + static_cast<std::ptrdiff_t>(directory_offset + offset));
    offset += parent.size();
    for (const auto &child : nodes) {
      if (&child == &nodes[0] || child.parent != index)
        continue;
      const auto name = std::as_bytes(std::span{child.name});
      const auto record = directory_record(child, name);
      if (offset + record.size() > sector_size) {
        return std::unexpected{
            make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                       "ISO9660 directory exceeds the narrow one-sector profile")};
      }
      std::ranges::copy(record, image_bytes.begin() +
                                    static_cast<std::ptrdiff_t>(directory_offset + offset));
      offset += record.size();
    }
  }
  for (const auto &node : nodes) {
    if (!node.directory) {
      std::ranges::copy(node.data, image_bytes.begin() +
                                       static_cast<std::ptrdiff_t>(node.sector * sector_size));
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
    return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                      "could not create temporary ISO9660 image")};
  output.write(reinterpret_cast<const char *>(image_bytes.data()),
               static_cast<std::streamsize>(image_bytes.size()));
  if (!output)
    return std::unexpected{make_error(ErrorCode::io_read_failed, ErrorCategory::io,
                                      "could not write temporary ISO9660 image")};
  return {};
}

} // namespace axk::detail
