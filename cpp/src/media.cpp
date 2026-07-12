#include "axklib/media.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <deque>
#include <format>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "axklib/bytes.hpp"

namespace axk {
namespace {

constexpr std::uint64_t iso_sector_size = 2048;
constexpr std::uint64_t iso_pvd_sector = 16;
constexpr std::string_view object_magic = "FSFSDEV3SPLX";

Error media_error(ErrorCode code, std::string message, std::string_view source = {},
                  std::optional<std::uint64_t> offset = std::nullopt) {
  ErrorContext context;
  if (!source.empty())
    context.source_path = std::string{source};
  context.raw_offset = offset;
  return make_error(code, ErrorCategory::container, std::move(message), std::move(context));
}

Result<std::vector<std::byte>> read_bytes(const RandomAccessReader &reader, std::uint64_t offset,
                                          std::size_t size, const CancellationToken &cancellation) {
  if (const auto check = cancellation.check(); !check)
    return std::unexpected{check.error()};
  std::vector<std::byte> result(size);
  if (const auto read = reader.read_exact_at(offset, result); !read)
    return std::unexpected{read.error()};
  if (const auto check = cancellation.check(); !check)
    return std::unexpected{check.error()};
  return result;
}

std::uint16_t le16(std::span<const std::byte> bytes, std::size_t offset) {
  const auto value = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
                     static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1]))
                         << 8U;
  return static_cast<std::uint16_t>(value);
}

std::uint32_t le32(std::span<const std::byte> bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(le16(bytes, offset)) |
         static_cast<std::uint32_t>(le16(bytes, offset + 2)) << 16U;
}

std::uint16_t be16(std::span<const std::byte> bytes, std::size_t offset) {
  const auto value = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset]))
                         << 8U |
                     static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1]));
  return static_cast<std::uint16_t>(value);
}

std::uint32_t be32(std::span<const std::byte> bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(be16(bytes, offset)) << 16U |
         static_cast<std::uint32_t>(be16(bytes, offset + 2));
}

bool power_of_two(std::uint32_t value) { return value != 0 && (value & (value - 1U)) == 0; }

std::string clean_ascii(std::span<const std::byte> bytes) {
  std::string result;
  result.reserve(bytes.size());
  for (const auto value : bytes) {
    const auto ch = std::to_integer<unsigned char>(value);
    if (ch == 0)
      break;
    result.push_back(ch >= 0x20 && ch <= 0x7e ? static_cast<char>(ch) : '?');
  }
  while (!result.empty() && result.back() == ' ')
    result.pop_back();
  return result;
}

bool object_prefix(std::span<const std::byte> bytes) {
  if (bytes.size() < object_magic.size())
    return false;
  return std::ranges::equal(bytes.first(object_magic.size()), object_magic, {},
                            [](std::byte value) { return static_cast<char>(value); });
}

struct MediaDecode {
  DecodedObject object;
  std::optional<Error> issue;
};

Result<MediaDecode> decode_media_object(std::span<const std::byte> bytes) {
  auto decoded = decode_object(bytes);
  if (decoded) {
    std::optional<Error> issue;
    if (std::holds_alternative<CurrentSmpl>(decoded->payload)) {
      const auto pcm_end = checked_add(decoded->header.header_size,
                                       decoded->header.payload_bytes_0x1c);
      if (!pcm_end || *pcm_end > bytes.size()) {
        issue = make_error(ErrorCode::object_malformed, ErrorCategory::object,
                           "SMPL stored PCM range exceeds the object payload");
      }
    }
    return MediaDecode{std::move(*decoded), std::move(issue)};
  }
  auto header = decode_object_header(bytes);
  if (!header)
    return std::unexpected{header.error()};
  std::vector<std::byte> raw{bytes.begin(), bytes.end()};
  return MediaDecode{DecodedObject{std::move(*header), ObjectFormat::unknown,
                                   GenericObject{std::move(raw)}},
                     decoded.error()};
}

std::string upper_ascii(std::string value) {
  std::ranges::transform(value, value.begin(),
                         [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
  return value;
}

bool unsafe_component(std::string_view value) {
  return value.empty() || value == "." || value == ".." ||
         value.find('/') != std::string_view::npos || value.find('\\') != std::string_view::npos ||
         std::ranges::any_of(value, [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
}

std::string decode_fat_name(std::span<const std::byte> entry) {
  auto stem = clean_ascii(entry.subspan(0, 8));
  auto extension = clean_ascii(entry.subspan(8, 3));
  if (std::to_integer<std::uint8_t>(entry[0]) == 0x05U && !stem.empty())
    stem[0] = static_cast<char>(0xe5);
  return extension.empty() ? stem : std::format("{}.{}", stem, extension);
}

std::uint16_t fat12_entry(std::span<const std::byte> fat, std::uint16_t cluster) {
  const auto offset = static_cast<std::size_t>(cluster) + cluster / 2U;
  const auto pair = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(fat[offset])) |
                    static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(fat[offset + 1]))
                        << 8U;
  const auto wide_pair = static_cast<std::uint32_t>(pair);
  return (cluster & 1U) != 0U ? static_cast<std::uint16_t>(wide_pair >> 4U)
                              : static_cast<std::uint16_t>(wide_pair & 0x0fffU);
}

Result<std::vector<std::uint16_t>> fat_chain(std::span<const std::byte> fat,
                                             const FatGeometry &geometry, std::uint16_t first,
                                             std::optional<std::uint32_t> required_bytes,
                                             std::string_view name, std::string_view source) {
  if (first < 2U || first >= geometry.data_cluster_count + 2U) {
    return std::unexpected{media_error(
        ErrorCode::container_invalid_geometry,
        std::format("FAT entry '{}' starts at invalid cluster {}", name, first), source)};
  }
  std::vector<std::uint16_t> result;
  std::unordered_set<std::uint16_t> seen;
  auto cluster = first;
  std::uint64_t capacity{};
  while (true) {
    if (cluster < 2U || cluster >= geometry.data_cluster_count + 2U) {
      return std::unexpected{media_error(
          ErrorCode::allocation_invalid_extent,
          std::format("FAT chain for '{}' leaves the data area at cluster {}", name, cluster),
          source)};
    }
    if (!seen.insert(cluster).second) {
      return std::unexpected{
          media_error(ErrorCode::allocation_cycle,
                      std::format("FAT chain loop in '{}' at cluster {}", name, cluster), source)};
    }
    result.push_back(cluster);
    capacity += geometry.cluster_size();
    const auto next = fat12_entry(fat, cluster);
    if (next >= 0xff8U)
      break;
    if (next == 0xff7U) {
      return std::unexpected{
          media_error(ErrorCode::allocation_invalid_extent,
                      std::format("FAT chain for '{}' reaches bad cluster marker", name), source)};
    }
    if (next == 0U || next == 1U || (next >= 0xff0U && next <= 0xff6U)) {
      return std::unexpected{media_error(
          ErrorCode::allocation_invalid_extent,
          std::format("FAT chain for '{}' has invalid successor 0x{:03x}", name, next), source)};
    }
    cluster = next;
    if (result.size() > geometry.data_cluster_count) {
      return std::unexpected{media_error(
          ErrorCode::allocation_cycle,
          std::format("FAT chain for '{}' exceeds the data cluster count", name), source)};
    }
  }
  if (required_bytes && capacity < *required_bytes) {
    return std::unexpected{
        media_error(ErrorCode::container_truncated,
                    std::format("FAT chain for '{}' contains {} bytes but file declares {}", name,
                                capacity, *required_bytes),
                    source)};
  }
  return result;
}

std::uint64_t fat_cluster_offset(const FatGeometry &geometry, std::uint16_t cluster) {
  return geometry.data_offset + static_cast<std::uint64_t>(cluster - 2U) * geometry.cluster_size();
}

struct PendingFatDirectory {
  std::string path;
  std::vector<std::byte> bytes;
  std::uint64_t offset{};
};

Result<void> scan_fat_directory(std::vector<FatFile> &files,
                                std::deque<PendingFatDirectory> &pending,
                                std::span<const std::byte> bytes, std::string_view parent,
                                std::uint64_t directory_offset, std::span<const std::byte> fat,
                                const FatGeometry &geometry, const RandomAccessReader &reader,
                                std::string_view source,
                                std::unordered_map<std::uint16_t, std::string> &claimed_clusters,
                                const CancellationToken &cancellation) {
  std::set<std::string> names;
  for (std::size_t offset = 0; offset + 32U <= bytes.size(); offset += 32U) {
    const auto entry = bytes.subspan(offset, 32);
    const auto first = std::to_integer<std::uint8_t>(entry[0]);
    if (first == 0U)
      break;
    if (first == 0xe5U)
      continue;
    const auto attributes = std::to_integer<std::uint8_t>(entry[0x0b]);
    if (attributes == 0x0fU || (attributes & 0x08U) != 0U)
      continue;
    const auto name = decode_fat_name(entry);
    const bool dot = name == "." || name == "..";
    if (!dot && unsafe_component(name)) {
      return std::unexpected{media_error(ErrorCode::container_invalid_geometry,
                                         std::format("unsafe FAT directory name '{}'", name),
                                         source, directory_offset + offset)};
    }
    const auto folded = upper_ascii(name);
    if (!dot && !names.insert(folded).second) {
      return std::unexpected{media_error(ErrorCode::container_invalid_geometry,
                                         std::format("duplicate FAT directory name '{}'", name),
                                         source, directory_offset + offset)};
    }
    if (dot)
      continue;
    const auto first_cluster = le16(entry, 0x1a);
    const auto size = le32(entry, 0x1c);
    const auto path = parent.empty() ? name : std::format("{}/{}", parent, name);
    if ((attributes & 0x10U) != 0U) {
      const auto chain = fat_chain(fat, geometry, first_cluster, std::nullopt, path, source);
      if (!chain)
        return std::unexpected{chain.error()};
      for (const auto cluster : *chain) {
        const auto [found, inserted] = claimed_clusters.emplace(cluster, path);
        if (!inserted) {
          return std::unexpected{
              media_error(ErrorCode::allocation_invalid_extent,
                          std::format("FAT entries '{}' and '{}' share cluster {}", found->second,
                                      path, cluster),
                          source)};
        }
      }
      std::vector<std::byte> child;
      child.reserve(chain->size() * geometry.cluster_size());
      for (const auto cluster : *chain) {
        const auto block = read_bytes(reader, fat_cluster_offset(geometry, cluster),
                                      geometry.cluster_size(), cancellation);
        if (!block)
          return std::unexpected{block.error()};
        child.insert(child.end(), block->begin(), block->end());
      }
      pending.push_back({path, std::move(child), fat_cluster_offset(geometry, first_cluster)});
      continue;
    }
    if (size == 0U)
      continue;
    const auto chain = fat_chain(fat, geometry, first_cluster, size, path, source);
    if (!chain)
      return std::unexpected{chain.error()};
    for (const auto cluster : *chain) {
      const auto [found, inserted] = claimed_clusters.emplace(cluster, path);
      if (!inserted) {
        return std::unexpected{media_error(
            ErrorCode::allocation_invalid_extent,
            std::format("FAT entries '{}' and '{}' share cluster {}", found->second, path, cluster),
            source)};
      }
    }
    files.push_back({path, name, directory_offset + offset, first_cluster, size, *chain,
                     fat_cluster_offset(geometry, first_cluster)});
  }
  return {};
}

struct IsoDirectoryRecord {
  std::uint32_t extent{};
  std::uint32_t size{};
  std::uint8_t flags{};
  std::string name;
};

Result<IsoDirectoryRecord> parse_iso_record(std::span<const std::byte> record,
                                            std::string_view source, std::uint64_t offset) {
  if (record.size() < 34U || std::to_integer<std::uint8_t>(record[0]) != record.size()) {
    return std::unexpected{media_error(ErrorCode::container_invalid_geometry,
                                       "malformed ISO9660 directory record", source, offset)};
  }
  const auto name_length = std::to_integer<std::uint8_t>(record[32]);
  if (33U + name_length > record.size()) {
    return std::unexpected{media_error(ErrorCode::container_truncated,
                                       "truncated ISO9660 directory record name", source, offset)};
  }
  const auto extent = le32(record, 2);
  const auto size = le32(record, 10);
  if (be32(record, 6) != extent || be32(record, 14) != size ||
      be16(record, 30) != le16(record, 28)) {
    return std::unexpected{media_error(ErrorCode::container_invalid_geometry,
                                       "ISO9660 both-endian directory fields disagree", source,
                                       offset)};
  }
  std::string name;
  if (name_length == 1U && std::to_integer<std::uint8_t>(record[33]) == 0U)
    name = ".";
  else if (name_length == 1U && std::to_integer<std::uint8_t>(record[33]) == 1U)
    name = "..";
  else {
    name = clean_ascii(record.subspan(33, name_length));
    if (const auto version = name.find(';'); version != std::string::npos)
      name.resize(version);
    while (!name.empty() && name.back() == '.')
      name.pop_back();
  }
  if (name != "." && name != ".." && unsafe_component(name)) {
    return std::unexpected{media_error(ErrorCode::container_invalid_geometry,
                                       std::format("unsafe ISO9660 name '{}'", name), source,
                                       offset)};
  }
  if ((std::to_integer<std::uint8_t>(record[25]) & 0x80U) != 0U) {
    return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                      "multi-extent ISO9660 files are unsupported")};
  }
  return IsoDirectoryRecord{extent, size, std::to_integer<std::uint8_t>(record[25]),
                            std::move(name)};
}

Result<void> validate_iso_extent(const RandomAccessReader &reader, std::uint32_t sector,
                                 std::uint32_t size, std::uint32_t volume_sectors,
                                 std::string_view source, bool require_physical_bytes = true) {
  const auto start = static_cast<std::uint64_t>(sector) * iso_sector_size;
  if (sector >= volume_sectors ||
      (require_physical_bytes &&
       (start > reader.size() || size > reader.size() - start)) ||
      (size != 0U && (static_cast<std::uint64_t>(size) + iso_sector_size - 1U) / iso_sector_size >
                         volume_sectors - sector)) {
    return std::unexpected{media_error(
        ErrorCode::allocation_invalid_extent,
        std::format("ISO9660 extent sector {} size {} is outside the volume", sector, size), source,
        start)};
  }
  return {};
}

std::vector<std::string> path_parts(std::string_view path) {
  std::vector<std::string> result;
  for (const auto part : std::views::split(path, '/'))
    result.emplace_back(part.begin(), part.end());
  return result;
}

bool f_directory(std::string_view value) {
  return value.size() == 4U && value[0] == 'F' &&
         std::ranges::all_of(value.substr(1),
                             [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

std::string lookup_label(const std::vector<std::pair<std::string, std::string>> &labels,
                         std::string_view key) {
  const auto found = std::ranges::find(labels, key, &std::pair<std::string, std::string>::first);
  return found == labels.end() ? std::string{} : found->second;
}

std::string object_category(ObjectType type) {
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
    return "UNKNOWN";
  }
  return "UNKNOWN";
}

std::string fallback_label(const MediaObject &object, std::span<const std::byte> bytes) {
  if (object.decoded.header.type == ObjectType::prog && bytes.size() >= 0x88U) {
    const auto display = clean_ascii(bytes.subspan(0x78, 16));
    if (!display.empty() && display != std::format("Pgm {}", object.decoded.header.name))
      return display;
    return {};
  }
  switch (object.decoded.header.type) {
  case ObjectType::sbac:
  case ObjectType::sbnk:
  case ObjectType::smpl:
  case ObjectType::sequ:
    return object.decoded.header.name;
  default:
    return {};
  }
}

template <typename T> const T *variant_ptr(const MediaStorage &storage) {
  return std::get_if<T>(&storage);
}

} // namespace

std::uint32_t FatGeometry::cluster_size() const noexcept {
  return static_cast<std::uint32_t>(bytes_per_sector) * sectors_per_cluster;
}

Result<FatImage> FatImage::open(std::shared_ptr<const RandomAccessReader> reader,
                                std::string source_name, const CancellationToken &cancellation) {
  if (!reader)
    return std::unexpected{media_error(ErrorCode::invalid_argument, "FAT reader is null")};
  const auto boot = read_bytes(*reader, 0, 512, cancellation);
  if (!boot)
    return std::unexpected{boot.error()};
  FatGeometry geometry;
  geometry.bytes_per_sector = le16(*boot, 0x0b);
  geometry.sectors_per_cluster = std::to_integer<std::uint8_t>((*boot)[0x0d]);
  geometry.reserved_sectors = le16(*boot, 0x0e);
  geometry.fat_count = std::to_integer<std::uint8_t>((*boot)[0x10]);
  geometry.root_entry_count = le16(*boot, 0x11);
  geometry.total_sectors = le16(*boot, 0x13);
  if (geometry.total_sectors == 0U)
    geometry.total_sectors = le32(*boot, 0x20);
  geometry.media_descriptor = std::to_integer<std::uint8_t>((*boot)[0x15]);
  geometry.sectors_per_fat = le16(*boot, 0x16);
  if (!power_of_two(geometry.bytes_per_sector) || geometry.bytes_per_sector < 512U ||
      geometry.bytes_per_sector > 4096U || !power_of_two(geometry.sectors_per_cluster) ||
      geometry.reserved_sectors == 0U || geometry.fat_count == 0U ||
      geometry.root_entry_count == 0U || geometry.sectors_per_fat == 0U ||
      geometry.total_sectors == 0U) {
    return std::unexpected{media_error(ErrorCode::container_invalid_geometry,
                                       "invalid or unsupported FAT BPB geometry", source_name)};
  }
  const auto root_sectors = (static_cast<std::uint32_t>(geometry.root_entry_count) * 32U +
                             geometry.bytes_per_sector - 1U) /
                            geometry.bytes_per_sector;
  const auto metadata_sectors =
      static_cast<std::uint64_t>(geometry.reserved_sectors) +
      static_cast<std::uint64_t>(geometry.fat_count) * geometry.sectors_per_fat + root_sectors;
  if (metadata_sectors >= geometry.total_sectors ||
      static_cast<std::uint64_t>(geometry.total_sectors) * geometry.bytes_per_sector >
          reader->size()) {
    return std::unexpected{media_error(ErrorCode::container_truncated,
                                       "FAT geometry exceeds the input image", source_name)};
  }
  geometry.data_cluster_count = static_cast<std::uint32_t>(
      (geometry.total_sectors - metadata_sectors) / geometry.sectors_per_cluster);
  if (geometry.data_cluster_count == 0U || geometry.data_cluster_count >= 4085U) {
    return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                      "unsupported FAT variant: only FAT12 is accepted")};
  }
  geometry.fat_offset =
      static_cast<std::uint64_t>(geometry.reserved_sectors) * geometry.bytes_per_sector;
  geometry.root_offset = static_cast<std::uint64_t>(geometry.reserved_sectors +
                                                    static_cast<std::uint32_t>(geometry.fat_count) *
                                                        geometry.sectors_per_fat) *
                         geometry.bytes_per_sector;
  geometry.data_offset =
      geometry.root_offset + static_cast<std::uint64_t>(root_sectors) * geometry.bytes_per_sector;
  const auto fat_size =
      static_cast<std::size_t>(geometry.sectors_per_fat) * geometry.bytes_per_sector;
  const auto fat = read_bytes(*reader, geometry.fat_offset, fat_size, cancellation);
  if (!fat)
    return std::unexpected{fat.error()};
  if (fat_size < 3U || std::to_integer<std::uint8_t>((*fat)[0]) != geometry.media_descriptor) {
    return std::unexpected{media_error(ErrorCode::container_invalid_geometry,
                                       "FAT media descriptor disagrees with the BPB", source_name)};
  }
  for (std::uint8_t index = 1; index < geometry.fat_count; ++index) {
    const auto copy =
        read_bytes(*reader, geometry.fat_offset + static_cast<std::uint64_t>(index) * fat_size,
                   fat_size, cancellation);
    if (!copy)
      return std::unexpected{copy.error()};
    if (*copy != *fat)
      return std::unexpected{
          media_error(ErrorCode::container_backup_mismatch, "FAT copies disagree", source_name)};
  }
  const auto root_size = static_cast<std::size_t>(root_sectors) * geometry.bytes_per_sector;
  const auto root = read_bytes(*reader, geometry.root_offset, root_size, cancellation);
  if (!root)
    return std::unexpected{root.error()};
  std::vector<FatFile> files;
  std::deque<PendingFatDirectory> pending;
  std::unordered_map<std::uint16_t, std::string> claimed_clusters;
  if (const auto scan =
          scan_fat_directory(files, pending, *root, {}, geometry.root_offset, *fat, geometry,
                             *reader, source_name, claimed_clusters, cancellation);
      !scan) {
    return std::unexpected{scan.error()};
  }
  std::unordered_set<std::string> visited_directories;
  while (!pending.empty()) {
    auto directory = std::move(pending.front());
    pending.pop_front();
    if (!visited_directories.insert(upper_ascii(directory.path)).second) {
      return std::unexpected{media_error(
          ErrorCode::allocation_cycle,
          std::format("duplicate or cyclic FAT directory '{}'", directory.path), source_name)};
    }
    if (const auto scan = scan_fat_directory(files, pending, directory.bytes, directory.path,
                                             directory.offset, *fat, geometry, *reader, source_name,
                                             claimed_clusters, cancellation);
        !scan) {
      return std::unexpected{scan.error()};
    }
  }
  std::ranges::sort(files, {}, &FatFile::path);
  FatImage result;
  result.reader_ = std::move(reader);
  result.source_name_ = std::move(source_name);
  result.geometry_ = geometry;
  result.files_ = std::move(files);
  return result;
}

Result<FatImage> FatImage::open(const std::filesystem::path &path,
                                const CancellationToken &cancellation) {
  auto reader = FileReader::open(path);
  if (!reader)
    return std::unexpected{reader.error()};
  return open(std::move(*reader), path.string(), cancellation);
}

const FatGeometry &FatImage::geometry() const noexcept { return geometry_; }
const std::string &FatImage::source_name() const noexcept { return source_name_; }
const std::vector<FatFile> &FatImage::files() const noexcept { return files_; }

Result<std::vector<std::byte>> FatImage::read_file(const FatFile &file,
                                                   const CancellationToken &cancellation) const {
  std::vector<std::byte> result;
  result.reserve(file.size);
  auto remaining = static_cast<std::size_t>(file.size);
  for (const auto cluster : file.clusters) {
    const auto take = std::min<std::size_t>(remaining, geometry_.cluster_size());
    const auto bytes =
        read_bytes(*reader_, fat_cluster_offset(geometry_, cluster), take, cancellation);
    if (!bytes)
      return std::unexpected{bytes.error()};
    result.insert(result.end(), bytes->begin(), bytes->end());
    remaining -= take;
    if (remaining == 0U)
      break;
  }
  if (remaining != 0U)
    return std::unexpected{media_error(ErrorCode::container_truncated,
                                       std::format("FAT file '{}' is truncated", file.path),
                                       source_name_)};
  return result;
}

Result<std::vector<MediaObject>> FatImage::objects(std::size_t maximum_object_bytes,
                                                   const CancellationToken &cancellation) const {
  std::vector<MediaObject> result;
  for (const auto &file : files_) {
    if (file.size > maximum_object_bytes)
      continue;
    auto bytes = read_file(file, cancellation);
    if (!bytes)
      return std::unexpected{bytes.error()};
    if (!object_prefix(*bytes))
      continue;
    auto decoded = decode_media_object(*bytes);
    if (!decoded)
      return std::unexpected{decoded.error()};
    result.push_back({std::format("fat:{}", file.path),
                      file.path,
                      "fat-root",
                      {},
                      {},
                      {"", LabelStatus::raw_identifier, "FAT root has no group label"},
                      {"FAT root", LabelStatus::confirmed, "FAT12 root directory"},
                      file.first_data_offset,
                      file.size,
                      std::move(decoded->object),
                      std::move(*bytes),
                      std::move(decoded->issue)});
  }
  return result;
}

Result<IsoImage> IsoImage::open(std::shared_ptr<const RandomAccessReader> reader,
                                std::string source_name, const CancellationToken &cancellation) {
  if (!reader)
    return std::unexpected{media_error(ErrorCode::invalid_argument, "ISO reader is null")};
  const auto pvd =
      read_bytes(*reader, iso_pvd_sector * iso_sector_size, iso_sector_size, cancellation);
  if (!pvd)
    return std::unexpected{pvd.error()};
  if (std::to_integer<std::uint8_t>((*pvd)[0]) != 1U ||
      clean_ascii(std::span{*pvd}.subspan(1, 5)) != "CD001" ||
      std::to_integer<std::uint8_t>((*pvd)[6]) != 1U) {
    return std::unexpected{media_error(ErrorCode::container_unrecognized,
                                       "primary ISO9660 volume descriptor not found at sector 16",
                                       source_name)};
  }
  const auto declared_volume_sectors = le32(*pvd, 80);
  if (be32(*pvd, 84) != declared_volume_sectors || le16(*pvd, 128) != iso_sector_size ||
      be16(*pvd, 130) != iso_sector_size || declared_volume_sectors == 0U) {
    return std::unexpected{media_error(ErrorCode::container_invalid_geometry,
                                       "invalid ISO9660 primary volume geometry", source_name)};
  }
  const auto physical_volume_sectors = reader->size() / iso_sector_size;
  if (physical_volume_sectors <= iso_pvd_sector) {
    return std::unexpected{media_error(ErrorCode::container_truncated,
                                       "ISO9660 image ends before its data volume", source_name)};
  }
  const bool declared_tail_is_missing = declared_volume_sectors > physical_volume_sectors;
  const auto root_length = std::to_integer<std::uint8_t>((*pvd)[156]);
  if (root_length < 34U || 156U + root_length > pvd->size()) {
    return std::unexpected{media_error(ErrorCode::container_invalid_geometry,
                                       "ISO9660 root directory record is malformed", source_name)};
  }
  const auto root = parse_iso_record(std::span{*pvd}.subspan(156, root_length), source_name,
                                     iso_pvd_sector * iso_sector_size + 156U);
  if (!root)
    return std::unexpected{root.error()};
  if (const auto valid =
          validate_iso_extent(*reader, root->extent, root->size, declared_volume_sectors,
                              source_name);
      !valid)
    return std::unexpected{valid.error()};
  struct Pending {
    std::string path;
    std::uint32_t extent;
    std::uint32_t size;
  };
  std::deque<Pending> pending{{"", root->extent, root->size}};
  std::set<std::pair<std::uint32_t, std::uint32_t>> visited;
  std::set<std::string> all_paths;
  std::vector<IsoFile> files;
  while (!pending.empty()) {
    const auto directory = std::move(pending.front());
    pending.pop_front();
    if (!visited.emplace(directory.extent, directory.size).second) {
      return std::unexpected{
          media_error(ErrorCode::allocation_cycle,
                      std::format("ISO9660 directory cycle at '{}'", directory.path), source_name)};
    }
    const auto bytes =
        read_bytes(*reader, static_cast<std::uint64_t>(directory.extent) * iso_sector_size,
                   directory.size, cancellation);
    if (!bytes)
      return std::unexpected{bytes.error()};
    std::set<std::string> names;
    std::size_t offset{};
    while (offset < bytes->size()) {
      const auto length = std::to_integer<std::uint8_t>((*bytes)[offset]);
      if (length == 0U) {
        offset = ((offset / iso_sector_size) + 1U) * iso_sector_size;
        continue;
      }
      if (offset % iso_sector_size + length > iso_sector_size || offset + length > bytes->size()) {
        return std::unexpected{media_error(
            ErrorCode::container_truncated, "ISO9660 directory record crosses its sector or extent",
            source_name, static_cast<std::uint64_t>(directory.extent) * iso_sector_size + offset)};
      }
      const auto record =
          parse_iso_record(std::span{*bytes}.subspan(offset, length), source_name,
                           static_cast<std::uint64_t>(directory.extent) * iso_sector_size + offset);
      if (!record)
        return std::unexpected{record.error()};
      offset += length;
      if (record->name == "." || record->name == "..")
        continue;
      if (!names.insert(upper_ascii(record->name)).second) {
        return std::unexpected{media_error(ErrorCode::container_invalid_geometry,
                                           std::format("duplicate ISO9660 name '{}'", record->name),
                                           source_name)};
      }
      const auto path = directory.path.empty() ? record->name
                                               : std::format("{}/{}", directory.path, record->name);
      if (!all_paths.insert(upper_ascii(path)).second) {
        return std::unexpected{media_error(ErrorCode::container_invalid_geometry,
                                           std::format("duplicate ISO9660 path '{}'", path),
                                           source_name)};
      }
      const bool is_directory = (record->flags & 0x02U) != 0U;
      if (const auto valid = validate_iso_extent(*reader, record->extent, record->size,
                                                 declared_volume_sectors, source_name,
                                                 is_directory || !declared_tail_is_missing);
          !valid)
        return std::unexpected{valid.error()};
      files.push_back({path, record->extent, record->size, is_directory});
      if (is_directory)
        pending.push_back({path, record->extent, record->size});
    }
  }
  std::ranges::sort(files, {}, &IsoFile::path);
  IsoImage result;
  result.reader_ = std::move(reader);
  result.source_name_ = std::move(source_name);
  result.volume_id_ = clean_ascii(std::span{*pvd}.subspan(40, 32));
  result.files_ = std::move(files);
  std::set<std::pair<std::string, std::string>> volumes;
  for (const auto &file : result.files_) {
    const auto parts = path_parts(file.path);
    if (file.is_directory && parts.size() == 2U && f_directory(parts[1]))
      volumes.emplace(parts[0], parts[1]);
  }
  for (const auto &[group, volume] : volumes) {
    for (const auto &file : result.files_) {
      const auto parts = path_parts(file.path);
      if (!file.is_directory && parts.size() == 2U && parts[0] == group && f_directory(parts[1]) &&
          file.size <= 64U) {
        auto label = result.read_file(file, cancellation);
        if (!label)
          return std::unexpected{label.error()};
        const auto value = clean_ascii(*label);
        if (!value.empty()) {
          result.group_labels_.emplace_back(group, value);
          break;
        }
      }
    }
    const auto table =
        std::ranges::find(result.files_, std::format("{}/0000", group), &IsoFile::path);
    if (table == result.files_.end() || table->is_directory)
      continue;
    auto bytes = result.read_file(*table, cancellation);
    if (!bytes)
      return std::unexpected{bytes.error()};
    for (std::size_t offset = 0; offset + 32U <= bytes->size(); offset += 32U) {
      const auto record = std::span{*bytes}.subspan(offset, 32);
      std::string id;
      for (std::size_t pos = 0; pos + 4U <= 24U; ++pos) {
        const auto candidate = clean_ascii(record.subspan(pos, 4));
        if (f_directory(candidate)) {
          id = candidate;
          break;
        }
      }
      const auto label = clean_ascii(record.subspan(1, 13));
      if (!id.empty() && !label.empty() && volumes.contains({group, id}))
        result.volume_labels_.emplace_back(std::format("{}/{}", group, id), label);
    }
  }
  return result;
}

Result<IsoImage> IsoImage::open(const std::filesystem::path &path,
                                const CancellationToken &cancellation) {
  auto reader = FileReader::open(path);
  if (!reader)
    return std::unexpected{reader.error()};
  return open(std::move(*reader), path.string(), cancellation);
}

const std::string &IsoImage::volume_id() const noexcept { return volume_id_; }
const std::string &IsoImage::source_name() const noexcept { return source_name_; }
const std::vector<IsoFile> &IsoImage::files() const noexcept { return files_; }

Result<std::vector<std::byte>> IsoImage::read_file(const IsoFile &file,
                                                   const CancellationToken &cancellation) const {
  return read_bytes(*reader_, static_cast<std::uint64_t>(file.extent_sector) * iso_sector_size,
                    file.size, cancellation);
}

Result<std::vector<MediaObject>> IsoImage::objects(std::size_t maximum_object_bytes,
                                                   const CancellationToken &cancellation) const {
  struct PendingObject {
    MediaObject object;
    std::vector<std::byte> bytes;
  };
  std::vector<PendingObject> pending;
  for (const auto &file : files_) {
    if (file.is_directory || file.size > maximum_object_bytes)
      continue;
    const auto file_offset = static_cast<std::uint64_t>(file.extent_sector) * iso_sector_size;
    if (file_offset > reader_->size() || file.size > reader_->size() - file_offset)
      continue;
    auto bytes = read_file(file, cancellation);
    if (!bytes)
      return std::unexpected{bytes.error()};
    if (!object_prefix(*bytes))
      continue;
    auto decoded = decode_media_object(*bytes);
    if (!decoded)
      return std::unexpected{decoded.error()};
    const auto parts = path_parts(file.path);
    const auto group = parts.empty() ? std::string{} : parts[0];
    const auto volume = parts.size() < 2U ? std::string{} : parts[1];
    auto group_label = lookup_label(group_labels_, group);
    auto volume_label = lookup_label(volume_labels_, std::format("{}/{}", group, volume));
    pending.push_back(
        {{std::format("iso9660:{}", file.path),
          file.path,
          std::format("iso:{}:iso9660", volume_id_),
          group,
          volume,
          {group_label.empty() ? group : group_label,
           group_label.empty() ? LabelStatus::raw_identifier : LabelStatus::confirmed,
           group_label.empty() ? "ISO9660 raw group identifier" : "Yamaha CD-ROM menu label"},
          {volume_label.empty() ? volume : volume_label,
           volume_label.empty() ? LabelStatus::raw_identifier : LabelStatus::confirmed,
           volume_label.empty() ? "ISO9660 raw volume identifier" : "Yamaha CD-ROM menu label"},
          static_cast<std::uint64_t>(file.extent_sector) * iso_sector_size,
          file.size,
          std::move(decoded->object),
          {},
          std::move(decoded->issue)},
         std::move(*bytes)});
  }
  const std::map<ObjectType, int> priority{{ObjectType::prog, 0},
                                           {ObjectType::sbac, 1},
                                           {ObjectType::sbnk, 2},
                                           {ObjectType::smpl, 3},
                                           {ObjectType::sequ, 4}};
  std::map<std::pair<std::string, std::string>, std::vector<PendingObject *>> scopes;
  for (auto &item : pending) {
    if (item.object.volume_label.status != LabelStatus::confirmed)
      scopes[{item.object.raw_group, item.object.raw_volume}].push_back(&item);
  }
  std::map<std::string, std::set<std::string>> used;
  for (auto &[scope, items] : scopes) {
    std::ranges::sort(items, [&](const PendingObject *left, const PendingObject *right) {
      return std::tuple{priority.contains(left->object.decoded.header.type)
                            ? priority.at(left->object.decoded.header.type)
                            : 99,
                        left->object.decoded.header.name, left->object.key} <
             std::tuple{priority.contains(right->object.decoded.header.type)
                            ? priority.at(right->object.decoded.header.type)
                            : 99,
                        right->object.decoded.header.name, right->object.key};
    });
    std::string derived;
    for (const auto *item : items) {
      derived = fallback_label(item->object, item->bytes);
      if (!derived.empty())
        break;
    }
    if (derived.empty())
      continue;
    auto display = derived;
    if (!used[scope.first].insert(upper_ascii(display)).second)
      display = std::format("{} ({})", display, scope.second);
    for (auto *item : items)
      item->object.volume_label = {display, LabelStatus::navigation_aid,
                                   "ISO directory path plus content-derived volume label fallback"};
  }
  std::vector<MediaObject> result;
  result.reserve(pending.size());
  for (auto &item : pending) {
    item.object.raw_payload = std::move(item.bytes);
    result.push_back(std::move(item.object));
  }
  std::ranges::sort(result, {}, &MediaObject::logical_path);
  return result;
}

Result<StandaloneObject> StandaloneObject::open(std::shared_ptr<const RandomAccessReader> reader,
                                                std::string source_name,
                                                std::size_t maximum_object_bytes) {
  if (!reader || reader->size() > maximum_object_bytes ||
      reader->size() > std::numeric_limits<std::size_t>::max()) {
    return std::unexpected{media_error(ErrorCode::io_unsupported_size,
                                       "standalone object exceeds the configured size limit",
                                       source_name)};
  }
  auto bytes = read_bytes(*reader, 0, static_cast<std::size_t>(reader->size()), {});
  if (!bytes)
    return std::unexpected{bytes.error()};
  if (!object_prefix(*bytes))
    return std::unexpected{media_error(ErrorCode::container_unrecognized,
                                       "file is not a standalone Yamaha object", source_name)};
  auto decoded = decode_media_object(*bytes);
  if (!decoded)
    return std::unexpected{decoded.error()};
  StandaloneObject result;
  result.object_ = {"standalone",
                    source_name,
                    "standalone",
                    {},
                    {},
                    {"", LabelStatus::raw_identifier, "standalone object has no group"},
                    {"Standalone object", LabelStatus::confirmed, "standalone object file"},
                    0,
                    reader->size(),
                    std::move(decoded->object),
                    std::move(*bytes),
                    std::move(decoded->issue)};
  return result;
}

Result<StandaloneObject> StandaloneObject::open(const std::filesystem::path &path,
                                                std::size_t maximum_object_bytes) {
  auto reader = FileReader::open(path);
  if (!reader)
    return std::unexpected{reader.error()};
  return open(std::move(*reader), path.string(), maximum_object_bytes);
}

const MediaObject &StandaloneObject::object() const noexcept { return object_; }

MediaContainer::MediaContainer(MediaStorage storage) : storage_{std::move(storage)} {}
MediaKind MediaContainer::kind() const noexcept {
  if (std::holds_alternative<Container>(storage_))
    return MediaKind::sfs;
  if (std::holds_alternative<FatImage>(storage_))
    return MediaKind::fat12_floppy;
  if (std::holds_alternative<IsoImage>(storage_))
    return MediaKind::iso9660;
  return MediaKind::standalone_object;
}
std::filesystem::path MediaContainer::source_path() const {
  if (const auto *sfs = std::get_if<Container>(&storage_))
    return sfs->source_path();
  if (const auto *fat = std::get_if<FatImage>(&storage_))
    return fat->source_name();
  if (const auto *iso = std::get_if<IsoImage>(&storage_))
    return iso->source_name();
  return std::get<StandaloneObject>(storage_).object().logical_path;
}
const MediaStorage &MediaContainer::storage() const noexcept { return storage_; }

Result<std::vector<MediaObject>>
MediaContainer::objects(std::size_t maximum_object_bytes,
                        const CancellationToken &cancellation) const {
  if (const auto *fat = variant_ptr<FatImage>(storage_))
    return fat->objects(maximum_object_bytes, cancellation);
  if (const auto *iso = variant_ptr<IsoImage>(storage_))
    return iso->objects(maximum_object_bytes, cancellation);
  if (const auto *standalone = variant_ptr<StandaloneObject>(storage_))
    return std::vector{standalone->object()};
  const auto *sfs = variant_ptr<Container>(storage_);
  auto catalog = build_object_catalog(*sfs, maximum_object_bytes, cancellation);
  if (!catalog)
    return std::unexpected{catalog.error()};
  std::vector<MediaObject> result;
  for (const auto &item : catalog->objects) {
    const auto placement = item.placement;
    auto bytes =
        sfs->read_record_data(item.partition, item.sfs_id, maximum_object_bytes, cancellation);
    if (!bytes)
      return std::unexpected{bytes.error()};
    result.push_back({item.key,
                      placement ? std::format("{}/{}/{}", placement->volume_name,
                                              placement->category_name, placement->entry_name)
                                : item.key,
                      item.scope_key,
                      {},
                      {},
                      {placement ? placement->partition_name : std::string{},
                       LabelStatus::confirmed, "SFS partition directory"},
                      {placement ? placement->volume_name : std::string{}, LabelStatus::confirmed,
                       "SFS volume directory"},
                      0,
                      bytes->size(),
                      item.object,
                      std::move(*bytes),
                      {}});
  }
  return result;
}

Result<MediaContainer> open_media(const std::filesystem::path &path,
                                  const CancellationToken &cancellation) {
  auto reader = FileReader::open(path);
  if (!reader)
    return std::unexpected{reader.error()};
  const auto prefix_size =
      static_cast<std::size_t>(std::min<std::uint64_t>((*reader)->size(), 0x9000U));
  auto prefix = read_bytes(**reader, 0, prefix_size, cancellation);
  if (!prefix)
    return std::unexpected{prefix.error()};
  if (object_prefix(*prefix)) {
    auto object = StandaloneObject::open(std::move(*reader), path.string());
    if (!object)
      return std::unexpected{object.error()};
    return MediaContainer{std::move(*object)};
  }
  if (prefix->size() >= iso_pvd_sector * iso_sector_size + 6U &&
      std::to_integer<std::uint8_t>((*prefix)[iso_pvd_sector * iso_sector_size]) == 1U &&
      clean_ascii(std::span{*prefix}.subspan(iso_pvd_sector * iso_sector_size + 1U, 5)) ==
          "CD001") {
    auto iso = IsoImage::open(std::move(*reader), path.string(), cancellation);
    if (!iso)
      return std::unexpected{iso.error()};
    return MediaContainer{std::move(*iso)};
  }
  if (prefix->size() >= 512U && le16(*prefix, 0x0b) >= 512U &&
      std::to_integer<std::uint8_t>((*prefix)[0x0d]) != 0U) {
    auto fat = FatImage::open(std::move(*reader), path.string(), cancellation);
    if (!fat)
      return std::unexpected{fat.error()};
    return MediaContainer{std::move(*fat)};
  }
  OpenOptions options;
  options.cancellation = cancellation;
  auto sfs = open_image(path, options);
  if (!sfs)
    return std::unexpected{sfs.error()};
  return MediaContainer{std::move(*sfs)};
}

Result<ObjectCatalog> build_object_catalog(const MediaContainer &container,
                                           std::size_t maximum_object_bytes,
                                           const CancellationToken &cancellation) {
  if (const auto *sfs = std::get_if<Container>(&container.storage()))
    return build_object_catalog(*sfs, maximum_object_bytes, cancellation);
  auto objects = container.objects(maximum_object_bytes, cancellation);
  if (!objects)
    return std::unexpected{objects.error()};
  ObjectCatalog result;
  std::map<std::pair<std::string, std::string>, std::uint32_t> volume_ids;
  std::uint32_t next_volume = 1;
  std::uint32_t next_object = 1;
  for (auto &object : *objects) {
    const auto scope = std::pair{object.raw_group, object.raw_volume};
    if (!volume_ids.contains(scope))
      volume_ids.emplace(scope, next_volume++);
    const auto id = next_object++;
    ObjectPlacement placement{PartitionIndex{0},
                              object.group_label.value,
                              SfsId{volume_ids.at(scope)},
                              object.volume_label.value,
                              object_category(object.decoded.header.type),
                              object.decoded.header.name,
                              object.raw_group.empty() ? std::string{}
                              : object.raw_volume.empty()
                                  ? object.raw_group
                                  : std::format("{}/{}", object.raw_group, object.raw_volume)};
    result.objects.push_back({object.key, PartitionIndex{0}, SfsId{id}, object.scope_key,
                              std::move(object.decoded), std::move(placement)});
    if (object.decode_issue) {
      result.issues.push_back({"media_object_decode_failed", render_error(*object.decode_issue),
                               PartitionIndex{0}, SfsId{id}});
    }
  }
  return result;
}

std::string sanitize_path_component(std::string_view value, std::string_view fallback) {
  std::string result;
  result.reserve(value.size());
  for (const auto ch : value) {
    const auto byte = static_cast<unsigned char>(ch);
    if (byte < 0x20U || byte == 0x7fU || ch == '/' || ch == '\\' || ch == ':' || ch == '*' ||
        ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|')
      result.push_back('_');
    else
      result.push_back(ch);
  }
  while (!result.empty() && (result.back() == ' ' || result.back() == '.'))
    result.pop_back();
  if (result.empty() || result == "." || result == "..")
    result = std::string{fallback};
  return result;
}

StructuredObjectPath structured_object_path(const MediaObject &object) {
  auto group = sanitize_path_component(object.group_label.value, "objects");
  auto volume = sanitize_path_component(object.volume_label.value, "volume");
  auto category = sanitize_path_component(object_category(object.decoded.header.type), "objects");
  auto name = sanitize_path_component(object.decoded.header.name, "unnamed");
  std::filesystem::path path{group};
  path /= volume;
  path /= category;
  path /= name;
  return {std::move(path), object.group_label, object.volume_label};
}

std::vector<StructuredObjectPath> structured_object_paths(std::span<const MediaObject> objects) {
  std::map<std::pair<std::string, std::string>, std::set<std::string>> raw_volumes;
  for (const auto &object : objects) {
    raw_volumes[{upper_ascii(object.group_label.value), upper_ascii(object.volume_label.value)}]
        .insert(upper_ascii(object.raw_volume));
  }
  std::vector<StructuredObjectPath> result;
  result.reserve(objects.size());
  for (const auto &object : objects) {
    auto path = structured_object_path(object);
    const auto &volumes = raw_volumes.at(
        {upper_ascii(object.group_label.value), upper_ascii(object.volume_label.value)});
    if (volumes.size() > 1U && !object.raw_volume.empty()) {
      auto group = sanitize_path_component(object.group_label.value, "objects");
      auto volume = sanitize_path_component(
          std::format("{} ({})", object.volume_label.value, object.raw_volume), "volume");
      auto category =
          sanitize_path_component(object_category(object.decoded.header.type), "objects");
      auto name = sanitize_path_component(object.decoded.header.name, "unnamed");
      path.relative_path = std::filesystem::path{group} / volume / category / name;
    }
    result.push_back(std::move(path));
  }
  return result;
}

} // namespace axk
