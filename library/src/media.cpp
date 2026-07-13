#include "axklib/media.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <ranges>

#include "axklib/utf8.hpp"
#include "media_internal.hpp"

namespace axk {
namespace detail {

Error media_error(ErrorCode code, std::string message, std::string_view source,
                  std::optional<std::uint64_t> offset) {
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

} // namespace detail

namespace {

template <typename T> const T *variant_ptr(const MediaStorage &storage) {
  return std::get_if<T>(&storage);
}

} // namespace

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
  auto prefix = detail::read_bytes(**reader, 0, prefix_size, cancellation);
  if (!prefix)
    return std::unexpected{prefix.error()};
  if (detail::object_prefix(*prefix)) {
    auto object = StandaloneObject::open(std::move(*reader), text::path_to_utf8(path));
    if (!object)
      return std::unexpected{object.error()};
    return MediaContainer{std::move(*object)};
  }
  if (prefix->size() >= detail::iso_pvd_sector * detail::iso_sector_size + 6U &&
      std::to_integer<std::uint8_t>((*prefix)[detail::iso_pvd_sector * detail::iso_sector_size]) ==
          1U &&
      detail::clean_ascii(std::span{*prefix}.subspan(
          detail::iso_pvd_sector * detail::iso_sector_size + 1U, 5)) == "CD001") {
    auto iso = IsoImage::open(std::move(*reader), text::path_to_utf8(path), cancellation);
    if (!iso)
      return std::unexpected{iso.error()};
    return MediaContainer{std::move(*iso)};
  }
  if (prefix->size() >= 512U && detail::le16(*prefix, 0x0b) >= 512U &&
      std::to_integer<std::uint8_t>((*prefix)[0x0d]) != 0U) {
    auto fat = FatImage::open(std::move(*reader), text::path_to_utf8(path), cancellation);
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

} // namespace axk
