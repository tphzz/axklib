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

enum class MediaKind : std::uint8_t {
    sfs,
    fat12_floppy,
    fat12_floppy_set,
    iso9660,
    a3k_archive,
    standalone_object,
    axk_object_directory
};
enum class LabelStatus : std::uint8_t { confirmed, navigation_aid, raw_identifier };
enum class MediaObjectReadMode : std::uint8_t { complete, decoded_metadata };
enum class FloppySetMarker : std::uint8_t { none, ordinary, continuation, final, invalid };
enum class FloppySetStatus : std::uint8_t { incomplete, complete };

struct YamahaFloppyCatalogEntry {
    std::uint16_t slot{};
    std::string logical_path;

    bool operator==(const YamahaFloppyCatalogEntry &) const = default;
};

struct YamahaFloppyCatalog {
    std::string disk_name;
    std::vector<YamahaFloppyCatalogEntry> files;
    std::vector<std::string> categories;
};

struct FloppyDiskIdentity {
    std::string label;
    std::string set_name;
    std::uint16_t index{};
    FloppySetMarker marker{FloppySetMarker::none};
    bool trusted_for_disk_set{};
};

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

struct A3kArchiveEntry {
    std::uint32_t ordinal{};
    std::string indexed_path;
    std::uint32_t offset{};
    std::uint32_t size{};

    bool operator==(const A3kArchiveEntry &) const = default;
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

struct AxkObjectDirectoryEntry {
    std::string name;
    std::shared_ptr<const RandomAccessReader> reader;
};

struct MediaObjectDescriptor {
    std::string key;
    std::string logical_path;
    std::string scope_key;
    std::string raw_group;
    std::string raw_volume;
    MenuLabel group_label;
    MenuLabel volume_label;
    std::uint64_t data_offset{};
    std::uint64_t size{};
};

struct MediaInventory {
    std::vector<MediaObjectDescriptor> objects;
    ObjectCatalog catalog;
    bool raw_payloads_complete{};
};

struct StructuredObjectPath {
    std::filesystem::path relative_path;
    MenuLabel group_label;
    MenuLabel volume_label;
};

// Read-only FAT12 profile for Yamaha A-series floppy media. This is not a
// general FAT implementation; FAT16, FAT32, exFAT, and filesystem writes are
// unsupported.
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
    [[nodiscard]] const std::optional<YamahaFloppyCatalog> &yamaha_catalog() const noexcept;
    [[nodiscard]] const FloppyDiskIdentity &disk_identity() const noexcept;
    [[nodiscard]] std::span<const MediaValidationIssue> validation_issues() const noexcept;
    [[nodiscard]] Result<std::vector<std::byte>> read_file(const FatFile &file,
                                                           const CancellationToken &cancellation = {}) const;
    [[nodiscard]] Result<std::vector<std::byte>> read_file_prefix(const FatFile &file, std::size_t maximum_bytes,
                                                                  const CancellationToken &cancellation = {}) const;
    [[nodiscard]] Result<std::vector<std::byte>> read_file_range(const FatFile &file, std::uint64_t offset,
                                                                 std::size_t size,
                                                                 const CancellationToken &cancellation = {}) const;
    [[nodiscard]] Result<std::vector<MediaObject>> objects(std::size_t maximum_object_bytes = 64U * 1024U * 1024U,
                                                           const CancellationToken &cancellation = {}) const;
    [[nodiscard]] Result<std::vector<MediaObject>> objects(MediaObjectReadMode mode,
                                                           std::size_t maximum_object_bytes = 64U * 1024U * 1024U,
                                                           const CancellationToken &cancellation = {}) const;

  private:
    std::shared_ptr<const RandomAccessReader> reader_;
    std::string source_name_;
    FatGeometry geometry_;
    std::vector<FatFile> files_;
    std::optional<YamahaFloppyCatalog> yamaha_catalog_;
    FloppyDiskIdentity disk_identity_;
    std::vector<MediaValidationIssue> validation_issues_;
};

// Read-only primary ISO9660 profile for Yamaha A-series CD-ROM media. Joliet
// and Rock Ridge metadata are not interpreted, and multi-extent files are
// unsupported.
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
    [[nodiscard]] std::span<const std::pair<std::string, std::string>> group_labels() const noexcept;
    [[nodiscard]] std::span<const std::pair<std::string, std::string>> volume_labels() const noexcept;
    [[nodiscard]] std::span<const MediaValidationIssue> validation_issues() const noexcept;
    [[nodiscard]] Result<std::vector<std::byte>> read_file(const IsoFile &file,
                                                           const CancellationToken &cancellation = {}) const;
    [[nodiscard]] Result<std::vector<std::byte>> read_file_prefix(const IsoFile &file, std::size_t maximum_bytes,
                                                                  const CancellationToken &cancellation = {}) const;
    [[nodiscard]] Result<std::vector<std::byte>> read_file_range(const IsoFile &file, std::uint64_t offset,
                                                                 std::size_t size,
                                                                 const CancellationToken &cancellation = {}) const;
    [[nodiscard]] Result<std::vector<MediaObject>> objects(std::size_t maximum_object_bytes = 64U * 1024U * 1024U,
                                                           const CancellationToken &cancellation = {}) const;
    [[nodiscard]] Result<std::vector<MediaObject>> objects(MediaObjectReadMode mode,
                                                           std::size_t maximum_object_bytes = 64U * 1024U * 1024U,
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

// Read-only A3K volume archive. The archive index contains complete
// Yamaha object files; its redundant path text is navigation metadata rather
// than authoritative object identity.
class AXK_API A3kArchive {
  public:
    static constexpr std::size_t maximum_entries = 1'024U;
    static constexpr std::size_t maximum_banner_bytes = 64U * 1'024U;

    [[nodiscard]] static Result<A3kArchive> open(std::shared_ptr<const RandomAccessReader> reader,
                                                 std::string source_name = {},
                                                 const CancellationToken &cancellation = {});
    [[nodiscard]] static Result<A3kArchive> open(const std::filesystem::path &path,
                                                 const CancellationToken &cancellation = {});

    [[nodiscard]] const std::string &source_name() const noexcept;
    [[nodiscard]] const std::string &banner() const noexcept;
    [[nodiscard]] const MenuLabel &volume_label() const noexcept;
    [[nodiscard]] std::span<const A3kArchiveEntry> entries() const noexcept;
    [[nodiscard]] std::span<const MediaValidationIssue> validation_issues() const noexcept;
    [[nodiscard]] Result<std::vector<std::byte>> read_entry(const A3kArchiveEntry &entry,
                                                            const CancellationToken &cancellation = {}) const;
    [[nodiscard]] Result<std::vector<std::byte>> read_entry_range(const A3kArchiveEntry &entry, std::uint64_t offset,
                                                                  std::size_t size,
                                                                  const CancellationToken &cancellation = {}) const;
    [[nodiscard]] Result<std::vector<std::byte>> read_entry_prefix(const A3kArchiveEntry &entry,
                                                                   std::size_t maximum_bytes,
                                                                   const CancellationToken &cancellation = {}) const;
    [[nodiscard]] Result<std::vector<MediaObject>> objects(std::size_t maximum_object_bytes = 64U * 1024U * 1024U,
                                                           const CancellationToken &cancellation = {}) const;
    [[nodiscard]] Result<std::vector<MediaObject>> objects(MediaObjectReadMode mode,
                                                           std::size_t maximum_object_bytes = 64U * 1024U * 1024U,
                                                           const CancellationToken &cancellation = {}) const;

  private:
    std::shared_ptr<const RandomAccessReader> reader_;
    std::string source_name_;
    std::string banner_;
    MenuLabel volume_label_;
    std::vector<A3kArchiveEntry> entries_;
    std::vector<MediaValidationIssue> validation_issues_;
};

class AXK_API FloppyDiskSet {
  public:
    static constexpr std::size_t maximum_members = 32U;

    [[nodiscard]] static Result<FloppyDiskSet> open(std::vector<FatImage> members, std::string source_name = {},
                                                    const CancellationToken &cancellation = {});
    [[nodiscard]] static Result<FloppyDiskSet> open_archive(std::shared_ptr<const RandomAccessReader> reader,
                                                            std::string source_name = {},
                                                            const CancellationToken &cancellation = {});

    [[nodiscard]] const std::string &source_name() const noexcept;
    [[nodiscard]] const std::vector<FatImage> &members() const noexcept;
    [[nodiscard]] FloppySetStatus status() const noexcept;
    [[nodiscard]] std::optional<std::uint16_t> next_required_index() const noexcept;
    [[nodiscard]] std::span<const MediaValidationIssue> validation_issues() const noexcept;
    [[nodiscard]] Result<std::vector<MediaObject>> objects(MediaObjectReadMode mode = MediaObjectReadMode::complete,
                                                           std::size_t maximum_object_bytes = 64U * 1024U * 1024U,
                                                           const CancellationToken &cancellation = {}) const;

  private:
    [[nodiscard]] static Result<FloppyDiskSet> open_members(std::vector<FatImage> members, std::string source_name,
                                                            bool allow_manifest_verified_ordinary_markers,
                                                            const CancellationToken &cancellation);

    std::string source_name_;
    std::vector<FatImage> members_;
    FloppySetStatus status_{FloppySetStatus::incomplete};
    std::optional<std::uint16_t> next_required_index_;
    std::vector<MediaValidationIssue> validation_issues_;
};

class AXK_API StandaloneObject {
  public:
    [[nodiscard]] static Result<StandaloneObject> open(std::shared_ptr<const RandomAccessReader> reader,
                                                       std::string source_name = {},
                                                       std::size_t maximum_object_bytes = 64U * 1024U * 1024U);
    [[nodiscard]] static Result<StandaloneObject> open(const std::filesystem::path &path,
                                                       std::size_t maximum_object_bytes = 64U * 1024U * 1024U);

    [[nodiscard]] const MediaObject &object() const noexcept;

  private:
    MediaObject object_;
};

// Read-only snapshot of one directory of Yamaha object files or a bounded
// one-level set of such directories. Filesystem and FAT metadata are
// intentionally not reconstructed.
class AXK_API AxkObjectDirectory {
  public:
    static constexpr std::size_t maximum_leaf_entries = 224U;
    static constexpr std::size_t maximum_entries = 1'024U;
    static constexpr std::uint64_t maximum_payload_bytes = 16U * 1024U * 1024U;
    static constexpr std::size_t maximum_depth = 2U;

    [[nodiscard]] static bool recognizes_entry_prefix(std::span<const std::byte> prefix, bool nested) noexcept;
    [[nodiscard]] static Result<bool> recognizes(std::vector<AxkObjectDirectoryEntry> entries,
                                                 std::string source_name = {},
                                                 const CancellationToken &cancellation = {});
    [[nodiscard]] static Result<AxkObjectDirectory> open(std::vector<AxkObjectDirectoryEntry> entries,
                                                         std::string source_name = {},
                                                         const CancellationToken &cancellation = {});
    [[nodiscard]] static Result<AxkObjectDirectory> open(const std::filesystem::path &path,
                                                         const CancellationToken &cancellation = {});

    [[nodiscard]] const std::string &source_name() const noexcept;
    [[nodiscard]] const std::vector<MediaObject> &stored_objects() const noexcept;
    [[nodiscard]] Result<std::vector<MediaObject>> objects(MediaObjectReadMode mode = MediaObjectReadMode::complete,
                                                           const CancellationToken &cancellation = {}) const;

  private:
    std::string source_name_;
    std::vector<MediaObject> objects_;
};

using MediaStorage =
    std::variant<Container, FatImage, FloppyDiskSet, IsoImage, A3kArchive, StandaloneObject, AxkObjectDirectory>;

class AXK_API MediaContainer {
  public:
    explicit MediaContainer(MediaStorage storage);

    [[nodiscard]] MediaKind kind() const noexcept;
    [[nodiscard]] std::filesystem::path source_path() const;
    [[nodiscard]] const MediaStorage &storage() const noexcept;
    [[nodiscard]] std::span<const MediaValidationIssue> validation_issues() const noexcept;
    [[nodiscard]] Result<std::vector<MediaObject>> objects(std::size_t maximum_object_bytes = 64U * 1024U * 1024U,
                                                           const CancellationToken &cancellation = {}) const;
    [[nodiscard]] Result<std::vector<MediaObject>> objects(MediaObjectReadMode mode,
                                                           std::size_t maximum_object_bytes = 64U * 1024U * 1024U,
                                                           const CancellationToken &cancellation = {}) const;

  private:
    MediaStorage storage_;
};

AXK_API Result<MediaContainer> open_media(const std::filesystem::path &path,
                                          const CancellationToken &cancellation = {});
AXK_API Result<MediaContainer> open_media(std::shared_ptr<const RandomAccessReader> reader,
                                          std::filesystem::path source_path,
                                          const CancellationToken &cancellation = {});
AXK_API Result<ObjectCatalog> build_object_catalog(const MediaContainer &container,
                                                   std::size_t maximum_object_bytes = 64U * 1024U * 1024U,
                                                   const CancellationToken &cancellation = {});
AXK_API Result<MediaInventory> build_media_inventory(const MediaContainer &container,
                                                     MediaObjectReadMode mode = MediaObjectReadMode::complete,
                                                     std::size_t maximum_object_bytes = 64U * 1024U * 1024U,
                                                     const CancellationToken &cancellation = {});
AXK_API Result<MediaObject> load_media_object(const MediaContainer &container, const MediaObjectDescriptor &descriptor,
                                              std::size_t maximum_object_bytes = 64U * 1024U * 1024U,
                                              const CancellationToken &cancellation = {});
AXK_API StructuredObjectPath structured_object_path(const MediaObject &object);
AXK_API std::vector<StructuredObjectPath> structured_object_paths(std::span<const MediaObject> objects);
AXK_API std::string sanitize_path_component(std::string_view value, std::string_view fallback);

} // namespace axk
