#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "axklib/object.hpp"
#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

namespace axk::detail {

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
    ObjectType type{ObjectType::unknown};
    std::string name;
    std::vector<std::byte> payload;
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
Result<WrittenMediaImage>
write_prepared_media_image(const PreparedMediaImage &image, const std::filesystem::path &output_path, bool overwrite,
                           const CancellationToken &cancellation,
                           const std::function<Result<void>(const std::filesystem::path &)> &validator = {});
Result<void> write_fat12_image(const PreparedMediaImage &image, const std::filesystem::path &temporary_path,
                               const CancellationToken &cancellation);
Result<std::uint32_t> checked_iso9660_sector_count(std::size_t directory_count,
                                                   std::span<const std::uint64_t> file_sizes);
Result<void> write_iso9660_image(const PreparedMediaImage &image, const std::filesystem::path &temporary_path,
                                 const CancellationToken &cancellation);

} // namespace axk::detail
