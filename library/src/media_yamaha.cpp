#include "axklib/media.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <tuple>

#include "axklib/bytes.hpp"
#include "axklib/utf8.hpp"
#include "media_internal.hpp"

namespace axk {
namespace {

std::string clean_label(std::span<const std::byte> bytes) {
  auto result = detail::clean_ascii(bytes);
  const auto first = result.find_first_not_of(' ');
  return first == std::string::npos ? std::string{} : result.substr(first);
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
      const auto pcm_end =
          checked_add(decoded->header.header_size, decoded->header.payload_bytes_0x1c);
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
  return MediaDecode{
      DecodedObject{std::move(*header), ObjectFormat::unknown, GenericObject{std::move(raw)}},
      decoded.error()};
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
    const auto display = detail::clean_ascii(bytes.subspan(0x78, 16));
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

} // namespace

namespace detail {

Result<IsoMenuLabels> read_yamaha_iso_menu_labels(const IsoImage &image,
                                                  const CancellationToken &cancellation) {
  IsoMenuLabels result;
  std::set<std::pair<std::string, std::string>> volumes;
  for (const auto &file : image.files()) {
    const auto parts = path_parts(file.path);
    if (file.is_directory && parts.size() == 2U && f_directory(parts[1]))
      volumes.emplace(parts[0], parts[1]);
  }
  for (const auto &[group, volume] : volumes) {
    for (const auto &file : image.files()) {
      const auto parts = path_parts(file.path);
      if (!file.is_directory && parts.size() == 2U && parts[0] == group && f_directory(parts[1]) &&
          file.size <= 64U) {
        auto label = image.read_file(file, cancellation);
        if (!label)
          return std::unexpected{label.error()};
        const auto value = clean_label(*label);
        if (!value.empty()) {
          result.groups.emplace_back(group, value);
          break;
        }
      }
    }
    const auto table =
        std::ranges::find(image.files(), std::format("{}/0000", group), &IsoFile::path);
    if (table == image.files().end() || table->is_directory)
      continue;
    auto bytes = image.read_file(*table, cancellation);
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
      const auto label = clean_label(record.subspan(1, 13));
      if (!id.empty() && !label.empty() && volumes.contains({group, id}))
        result.volumes.emplace_back(std::format("{}/{}", group, id), label);
    }
  }
  return result;
}

} // namespace detail

Result<std::vector<MediaObject>> FatImage::objects(std::size_t maximum_object_bytes,
                                                   const CancellationToken &cancellation) const {
  std::vector<MediaObject> result;
  for (const auto &file : files_) {
    if (file.size > maximum_object_bytes)
      continue;
    auto bytes = read_file(file, cancellation);
    if (!bytes)
      return std::unexpected{bytes.error()};
    if (!detail::object_prefix(*bytes))
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
    const auto file_offset =
        static_cast<std::uint64_t>(file.extent_sector) * detail::iso_sector_size;
    if (file_offset > reader_->size() || file.size > reader_->size() - file_offset)
      continue;
    auto bytes = read_file(file, cancellation);
    if (!bytes)
      return std::unexpected{bytes.error()};
    if (!detail::object_prefix(*bytes))
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
          static_cast<std::uint64_t>(file.extent_sector) * detail::iso_sector_size,
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
    if (!used[scope.first].insert(detail::upper_ascii(display)).second)
      display = std::format("{} ({})", display, scope.second);
    for (auto *item : items) {
      item->object.volume_label = {display, LabelStatus::navigation_aid,
                                   "ISO directory path plus content-derived volume label fallback"};
    }
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
    return std::unexpected{
        detail::media_error(ErrorCode::io_unsupported_size,
                            "standalone object exceeds the configured size limit", source_name)};
  }
  auto bytes = detail::read_bytes(*reader, 0, static_cast<std::size_t>(reader->size()), {});
  if (!bytes)
    return std::unexpected{bytes.error()};
  if (!detail::object_prefix(*bytes)) {
    return std::unexpected{detail::media_error(
        ErrorCode::container_unrecognized, "file is not a standalone Yamaha object", source_name)};
  }
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
  return open(std::move(*reader), text::path_to_utf8(path), maximum_object_bytes);
}

const MediaObject &StandaloneObject::object() const noexcept { return object_; }

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
  auto text = std::string{value};
  const auto first = text.find_first_not_of(" \t\r\n");
  const auto last = text.find_last_not_of(" \t\r\n");
  text = first == std::string::npos ? std::string{fallback} : text.substr(first, last - first + 1U);
  std::size_t duplicate_count{};
  while (!text.empty() && text.back() == '*') {
    ++duplicate_count;
    text.pop_back();
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
    text.pop_back();

  std::string result;
  result.reserve(text.size());
  bool previous_space{};
  bool previous_underscore{};
  for (const auto ch : text) {
    if (ch == '<' || ch == '>') {
      result += ch == '<' ? "_lt_" : "_gt_";
      previous_space = false;
      previous_underscore = true;
      continue;
    }
    const auto byte = static_cast<unsigned char>(ch);
    const auto invalid = byte < 0x20U || byte == 0x7fU || std::string_view{"/:*?\"|"}.contains(ch);
    if (invalid || ch == '_') {
      if (!previous_underscore)
        result.push_back('_');
      previous_space = false;
      previous_underscore = true;
    } else if (std::isspace(byte) != 0) {
      if (!result.empty() && !previous_space)
        result.push_back(' ');
      previous_space = true;
      previous_underscore = false;
    } else {
      result.push_back(ch);
      previous_space = false;
      previous_underscore = false;
    }
  }
  while (!result.empty() && (result.back() == ' ' || result.back() == '.' || result.back() == '_'))
    result.pop_back();
  while (!result.empty() &&
         (result.front() == ' ' || result.front() == '.' || result.front() == '_'))
    result.erase(result.begin());
  if (result.empty() || result == "." || result == "..")
    result = std::string{fallback};
  if (duplicate_count != 0U)
    result += std::format(" ({})", duplicate_count + 1U);
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
    raw_volumes[{detail::upper_ascii(object.group_label.value),
                 detail::upper_ascii(object.volume_label.value)}]
        .insert(detail::upper_ascii(object.raw_volume));
  }
  std::vector<StructuredObjectPath> result;
  result.reserve(objects.size());
  for (const auto &object : objects) {
    auto path = structured_object_path(object);
    const auto &volumes = raw_volumes.at({detail::upper_ascii(object.group_label.value),
                                          detail::upper_ascii(object.volume_label.value)});
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
