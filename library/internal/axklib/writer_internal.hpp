#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "axklib/floppy_catalog_internal.hpp"
#include "axklib/media.hpp"
#include "axklib/object.hpp"
#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

namespace axk::detail {

class TemporaryPublication;

inline constexpr std::size_t sfs_directory_index_page_bytes = 1024U;
inline constexpr std::size_t sfs_directory_index_record_bytes = 72U;
inline constexpr std::size_t sfs_directory_index_records_per_page = 14U;
inline constexpr std::uint32_t sfs_directory_index_capacity = 5012U;
inline constexpr std::uint32_t sfs_directory_index_page_capacity = 358U;

enum class RecordKind : std::uint8_t { hidden, system, directory, object };

struct PreparedRecord {
    std::uint32_t id{};
    std::vector<std::byte> payload;
    RecordKind kind{};
    std::uint16_t tail{};
    std::uint32_t cluster{};
    std::uint32_t clusters{};
};

struct PreparedWaveformMember {
    std::string name;
    std::uint32_t reference_value{};
    std::uint32_t sample_rate{};
    std::uint32_t frame_count{};
};

struct PreparedMediaObject {
    PreparedMediaObject() = default;
    PreparedMediaObject(ObjectType object_type, std::string object_name, std::vector<std::byte> bytes)
        : type(object_type), name(std::move(object_name)), payload(std::make_shared<MemoryReader>(std::move(bytes))) {}
    PreparedMediaObject(ObjectType object_type, std::string object_name,
                        std::shared_ptr<const RandomAccessReader> payload_reader)
        : type(object_type), name(std::move(object_name)), payload(std::move(payload_reader)) {}

    ObjectType type{ObjectType::unknown};
    std::string name;
    std::shared_ptr<const RandomAccessReader> payload;
    std::string fat_filename;

    [[nodiscard]] std::uint64_t size() const noexcept { return payload == nullptr ? 0U : payload->size(); }
};

struct PreparedMediaFile {
    std::string path;
    std::vector<std::byte> payload;
};

struct PreparedIsoVolume {
    std::string raw_group;
    std::string group_name;
    std::string raw_volume;
    std::string volume_name;
    std::vector<PreparedMediaObject> objects;
};

struct PreparedMediaImage {
    MediaBuildManifest manifest;
    MediaBuildLimits limits{};
    std::vector<PreparedMediaObject> objects;
    std::vector<PreparedMediaFile> retained_files;
    std::vector<PreparedIsoVolume> iso_volumes;
    std::optional<YamahaFloppyCatalog> floppy_catalog;
    struct SampleWaveDependency {
        std::size_t sample_object_index{};
        std::size_t wave_data_object_index{};

        bool operator==(const SampleWaveDependency &) const = default;
    };
    std::vector<SampleWaveDependency> sample_wave_dependencies;
};

struct Iso9660LayoutNode {
    std::string name;
    bool directory{};
    std::uint32_t sector{};
    std::uint32_t extent_size{};
    std::size_t parent{};
    std::vector<std::byte> owned_data;
    std::shared_ptr<const RandomAccessReader> external_data;

    [[nodiscard]] std::uint64_t payload_size() const noexcept {
        return external_data == nullptr ? owned_data.size() : external_data->size();
    }
};

struct Iso9660Layout {
    std::vector<Iso9660LayoutNode> nodes;
    std::vector<std::size_t> directory_indices;
    std::vector<std::byte> little_path_table;
    std::vector<std::byte> big_path_table;
    std::uint32_t little_path_sector{};
    std::uint32_t big_path_sector{};
    std::uint32_t sector_count{};
    std::uint64_t output_bytes{};
};

struct PreparedMediaConversion {
    PreparedMediaImage image;
    MediaConversionPlanSummary summary;
};

struct FloppyObjectSegment {
    std::size_t object_index{};
    std::uint16_t catalog_slot{};
    std::uint64_t payload_offset{};
    std::uint64_t payload_bytes{};
    std::uint32_t header_bytes{};
    bool split{};
    bool catalog_member_suffix{};
};

struct FloppyDiskLayout {
    std::string name;
    std::string marker_name;
    std::uint16_t marker_slot{};
    std::vector<FloppyObjectSegment> segments;
};

struct FloppyDiskSetPlan {
    std::vector<FloppyDiskLayout> disks;
    std::uint64_t projected_archive_bytes{};
};

Result<std::vector<std::byte>> prepare_smpl_payload(const WaveformSpec &spec, const ImportedAudio &audio,
                                                    std::uint32_t reference_value);
Result<std::vector<std::byte>> prepare_sbnk_payload(const SampleSpec &spec, const PreparedWaveformMember &left,
                                                    const std::optional<PreparedWaveformMember> &right = {},
                                                    bool sample_bank_member = false,
                                                    const std::vector<std::uint8_t> &linked_programs = {});
Result<std::vector<std::byte>> prepare_sbac_payload(const SampleBankSpec &sample_bank,
                                                    const std::map<std::string, SampleSpec> &samples);
Result<std::vector<std::byte>> prepare_prog_payload(const ProgramSpec &program);
Result<std::vector<std::byte>> encode_sfs_index_record(const PreparedRecord &record);
Result<std::vector<std::byte>> encode_sfs_index_record(const PreparedRecord &record, std::span<const Extent> extents,
                                                       std::uint32_t size,
                                                       std::span<const std::uint32_t> continuation_clusters = {});

Result<std::vector<PreparedRecord>> prepare_partition_records(const PartitionSpec &partition,
                                                              const PartitionGeometry &geometry,
                                                              std::size_t partition_count,
                                                              const CancellationToken &cancellation);
Result<std::size_t> checked_directory_index_size(std::span<const PreparedRecord> records);

Result<PreparedMediaImage> prepare_media_image(const MediaBuildManifest &manifest, const MediaBuildLimits &limits,
                                               const CancellationToken &cancellation);
Result<PreparedMediaConversion> prepare_media_conversion(std::shared_ptr<const RandomAccessReader> source_reader,
                                                         const std::filesystem::path &source_path,
                                                         const MediaConversionRequest &request,
                                                         const MediaBuildLimits &limits,
                                                         const CancellationToken &cancellation);
Result<WrittenMediaImage>
write_prepared_media_image(const PreparedMediaImage &image, const std::filesystem::path &output_path, bool overwrite,
                           const CancellationToken &cancellation,
                           const std::function<Result<void>(const std::filesystem::path &)> &validator = {});
Result<void> write_fat12_image(const PreparedMediaImage &image, TemporaryPublication &publication,
                               const CancellationToken &cancellation);
Result<std::vector<std::byte>> build_fat12_image(const PreparedMediaImage &image,
                                                 const CancellationToken &cancellation);
Result<std::vector<std::string>> plan_fat12_object_filenames(const PreparedMediaImage &image);
Result<std::string> yamaha_floppy_disk_name(std::string_view volume_name, std::size_t disk_index);
std::vector<std::string> yamaha_floppy_categories(std::span<const PreparedMediaObject> objects);
Result<std::string> yamaha_floppy_object_path(ObjectType type, std::string_view name,
                                              std::optional<std::size_t> disk_index = {});
Result<std::string> yamaha_floppy_physical_filename(std::string_view logical_name, std::uint16_t slot);
Result<FloppyDiskSetPlan> plan_floppy_disk_set(const PreparedMediaImage &image, std::string_view volume_name,
                                               const CancellationToken &cancellation);
Result<WrittenMediaImage> write_floppy_disk_set(const PreparedMediaImage &image, const FloppyDiskSetPlan &plan,
                                                const std::filesystem::path &output_path, bool overwrite,
                                                const CancellationToken &cancellation);
Result<Iso9660Layout> plan_iso9660_layout(const PreparedMediaImage &image);
Result<void> write_iso9660_image(const PreparedMediaImage &image, TemporaryPublication &publication,
                                 const CancellationToken &cancellation);

} // namespace axk::detail
