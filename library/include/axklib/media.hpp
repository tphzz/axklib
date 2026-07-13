#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "axklib/catalog.hpp"
#include "axklib/error.hpp"
#include "axklib/io.hpp"
#include "axklib/object.hpp"
#include "axklib/sfs.hpp"

namespace axk {

enum class MediaKind : std::uint8_t { sfs, fat12_floppy, iso9660, standalone_object };
enum class LabelStatus : std::uint8_t { confirmed, navigation_aid, raw_identifier };

struct FatGeometry {
  std::uint16_t bytes_per_sector{};
  std::uint8_t sectors_per_cluster{};
  std::uint16_t reserved_sectors{};
  std::uint8_t fat_count{};
  std::uint16_t root_entry_count{};
  std::uint32_t total_sectors{};
  std::uint8_t media_descriptor{};
  std::uint16_t sectors_per_fat{};
  std::uint64_t fat_offset{};
  std::uint64_t root_offset{};
  std::uint64_t data_offset{};
  std::uint32_t data_cluster_count{};

  [[nodiscard]] std::uint32_t cluster_size() const noexcept;
};

struct FatFile {
  std::string path;
  std::string name;
  std::uint64_t directory_offset{};
  std::uint16_t first_cluster{};
  std::uint32_t size{};
  std::vector<std::uint16_t> clusters;
  std::uint64_t first_data_offset{};
};

struct IsoFile {
  std::string path;
  std::uint32_t extent_sector{};
  std::uint32_t size{};
  bool is_directory{};
};

struct MenuLabel {
  std::string value;
  LabelStatus status{LabelStatus::raw_identifier};
  std::string basis;
};

struct MediaValidationIssue {
  std::string code;
  std::string message;
  std::string sampler_path;
  std::string basis;
  std::string recommended_next_check;
};

struct MediaObject {
  std::string key;
  std::string logical_path;
  std::string scope_key;
  std::string raw_group;
  std::string raw_volume;
  MenuLabel group_label;
  MenuLabel volume_label;
  std::uint64_t data_offset{};
  std::uint64_t size{};
  DecodedObject decoded;
  std::vector<std::byte> raw_payload;
  std::optional<Error> decode_issue;
};

struct StructuredObjectPath {
  std::filesystem::path relative_path;
  MenuLabel group_label;
  MenuLabel volume_label;
};

// Read-only FAT12 profile for Yamaha A-series floppy media. This is not a
// general FAT implementation; FAT16, FAT32, exFAT, and filesystem writes are unsupported.
class AXK_API FatImage {
public:
  [[nodiscard]] static Result<FatImage> open(std::shared_ptr<const RandomAccessReader> reader,
                                             std::string source_name = {},
                                             const CancellationToken &cancellation = {});
  [[nodiscard]] static Result<FatImage> open(const std::filesystem::path &path,
                                             const CancellationToken &cancellation = {});

  [[nodiscard]] const FatGeometry &geometry() const noexcept;
  [[nodiscard]] const std::string &source_name() const noexcept;
  [[nodiscard]] const std::vector<FatFile> &files() const noexcept;
  [[nodiscard]] Result<std::vector<std::byte>>
  read_file(const FatFile &file, const CancellationToken &cancellation = {}) const;
  [[nodiscard]] Result<std::vector<MediaObject>>
  objects(std::size_t maximum_object_bytes = 64U * 1024U * 1024U,
          const CancellationToken &cancellation = {}) const;

private:
  std::shared_ptr<const RandomAccessReader> reader_;
  std::string source_name_;
  FatGeometry geometry_;
  std::vector<FatFile> files_;
};

// Read-only primary ISO9660 profile for Yamaha A-series CD-ROM media. Joliet
// and Rock Ridge metadata are not interpreted, and multi-extent files are unsupported.
class AXK_API IsoImage {
public:
  [[nodiscard]] static Result<IsoImage> open(std::shared_ptr<const RandomAccessReader> reader,
                                             std::string source_name = {},
                                             const CancellationToken &cancellation = {});
  [[nodiscard]] static Result<IsoImage> open(const std::filesystem::path &path,
                                             const CancellationToken &cancellation = {});

  [[nodiscard]] const std::string &volume_id() const noexcept;
  [[nodiscard]] const std::string &source_name() const noexcept;
  [[nodiscard]] const std::vector<IsoFile> &files() const noexcept;
  [[nodiscard]] std::span<const MediaValidationIssue> validation_issues() const noexcept;
  [[nodiscard]] Result<std::vector<std::byte>>
  read_file(const IsoFile &file, const CancellationToken &cancellation = {}) const;
  [[nodiscard]] Result<std::vector<MediaObject>>
  objects(std::size_t maximum_object_bytes = 64U * 1024U * 1024U,
          const CancellationToken &cancellation = {}) const;

private:
  std::shared_ptr<const RandomAccessReader> reader_;
  std::string source_name_;
  std::string volume_id_;
  std::vector<IsoFile> files_;
  std::vector<std::pair<std::string, std::string>> group_labels_;
  std::vector<std::pair<std::string, std::string>> volume_labels_;
  std::vector<MediaValidationIssue> validation_issues_;
};

class AXK_API StandaloneObject {
public:
  [[nodiscard]] static Result<StandaloneObject>
  open(std::shared_ptr<const RandomAccessReader> reader, std::string source_name = {},
       std::size_t maximum_object_bytes = 64U * 1024U * 1024U);
  [[nodiscard]] static Result<StandaloneObject>
  open(const std::filesystem::path &path, std::size_t maximum_object_bytes = 64U * 1024U * 1024U);

  [[nodiscard]] const MediaObject &object() const noexcept;

private:
  MediaObject object_;
};

using MediaStorage = std::variant<Container, FatImage, IsoImage, StandaloneObject>;

class AXK_API MediaContainer {
public:
  explicit MediaContainer(MediaStorage storage);

  [[nodiscard]] MediaKind kind() const noexcept;
  [[nodiscard]] std::filesystem::path source_path() const;
  [[nodiscard]] const MediaStorage &storage() const noexcept;
  [[nodiscard]] std::span<const MediaValidationIssue> validation_issues() const noexcept;
  [[nodiscard]] Result<std::vector<MediaObject>>
  objects(std::size_t maximum_object_bytes = 64U * 1024U * 1024U,
          const CancellationToken &cancellation = {}) const;

private:
  MediaStorage storage_;
};

AXK_API Result<MediaContainer> open_media(const std::filesystem::path &path,
                                          const CancellationToken &cancellation = {});
AXK_API Result<ObjectCatalog> build_object_catalog(const MediaContainer &container,
                                                   std::size_t maximum_object_bytes = 64U * 1024U *
                                                                                      1024U,
                                                   const CancellationToken &cancellation = {});
AXK_API StructuredObjectPath structured_object_path(const MediaObject &object);
AXK_API std::vector<StructuredObjectPath>
structured_object_paths(std::span<const MediaObject> objects);
AXK_API std::string sanitize_path_component(std::string_view value, std::string_view fallback);

} // namespace axk
