#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/io.hpp"
#include "axklib/media.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/writer.hpp"
#include "axklib/writer_internal.hpp"

namespace {

class SparseReader final : public axk::RandomAccessReader {
  public:
    explicit SparseReader(std::uint64_t size) : size_(size) {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

    [[nodiscard]] axk::Result<void> read_exact_at(std::uint64_t offset,
                                                  std::span<std::byte> destination) const override {
        if (offset > size_ || destination.size() > size_ - offset) {
            return std::unexpected{
                axk::make_error(axk::ErrorCode::io_short_read, axk::ErrorCategory::io, "sparse read is out of range")};
        }
        std::ranges::fill(destination, std::byte{});
        return {};
    }

  private:
    std::uint64_t size_{};
};

axk::VolumeSpec source_volume(const std::filesystem::path &audio_path, std::string name = "Source Volume") {
    axk::VolumeSpec volume;
    volume.name = std::move(name);
    volume.waveforms.push_back({"wave", "Source Wave", audio_path, 60U, {}});

    axk::SampleSpec sample;
    sample.name = "Source Sample";
    sample.waveform_id = "wave";
    sample.root_key = 60U;
    sample.key_high = 127U;
    volume.samples.push_back(std::move(sample));
    axk::SampleSpec direct;
    direct.name = "Direct Sample";
    direct.waveform_id = "wave";
    direct.root_key = 60U;
    direct.key_high = 127U;
    volume.samples.push_back(std::move(direct));
    volume.sample_banks.push_back({"Source Bank", {"Source Sample"}});
    volume.programs.push_back({1U, {{"SBAC", "Source Bank", 1U}, {"SBNK", "Direct Sample", 2U}}});
    return volume;
}

axk::VolumeSpec dense_source_volume(const std::filesystem::path &audio_path) {
    axk::VolumeSpec volume;
    volume.name = "Dense Volume";
    volume.waveforms.push_back({"wave", "Shared Wave", audio_path, 60U, {}});
    for (std::size_t index = 1U; index <= 203U; ++index) {
        axk::SampleSpec sample;
        sample.name = std::format("Sample {:03}", index);
        sample.waveform_id = "wave";
        sample.root_key = 60U;
        sample.key_high = 127U;
        volume.samples.push_back(std::move(sample));
    }
    for (std::uint8_t number = 1U; number <= 64U; ++number) {
        const auto bank_name = std::format("Bank {:03}", number);
        volume.sample_banks.push_back({bank_name, {volume.samples[number - 1U].name}});
        volume.programs.push_back(
            {number, {{"SBAC", bank_name, 1U}, {"SBNK", volume.samples[64U + number - 1U].name, 2U}}});
    }
    return volume;
}

void make_program_assignment_target_missing_with_same_type_context(const std::filesystem::path &path) {
    const auto media = axk::open_media(path);
    ASSERT_TRUE(media) << media.error().message;
    const auto *sfs = std::get_if<axk::Container>(&media->storage());
    ASSERT_NE(sfs, nullptr);
    ASSERT_FALSE(sfs->partitions().empty());
    const auto &partition = sfs->partitions().front();
    const auto program =
        std::ranges::find_if(partition.records, [](const auto &record) { return record.object_type == "PROG"; });
    ASSERT_NE(program, partition.records.end());
    ASSERT_EQ(program->extents.size(), 1U);
    const auto row_offset =
        (static_cast<std::uint64_t>(partition.start_sector) +
         static_cast<std::uint64_t>(program->extents.front().cluster_offset) * partition.sectors_per_cluster) *
            512U +
        0x120U;
    std::fstream image{path, std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(image);
    image.seekp(static_cast<std::streamoff>(row_offset + 0x0fU));
    image.put('*');
    const auto context_offset = row_offset + 0x38U;
    image.seekp(static_cast<std::streamoff>(context_offset));
    image.write("Source Bank     ", 16);
    image.seekp(static_cast<std::streamoff>(context_offset + 0x14U));
    image.put(static_cast<char>(0x11));
    ASSERT_TRUE(image);
}

std::vector<std::vector<std::byte>> payloads(const axk::MediaContainer &media) {
    const auto objects = media.objects();
    EXPECT_TRUE(objects) << objects.error().message;
    std::vector<std::vector<std::byte>> result;
    if (!objects)
        return result;
    for (const auto &object : *objects)
        result.push_back(object.raw_payload);
    std::ranges::sort(result, [](const auto &left, const auto &right) {
        return std::lexicographical_compare(
            left.begin(), left.end(), right.begin(), right.end(), [](std::byte lhs, std::byte rhs) {
                return std::to_integer<unsigned int>(lhs) < std::to_integer<unsigned int>(rhs);
            });
    });
    return result;
}

std::shared_ptr<const axk::RandomAccessReader> open_reader(const std::filesystem::path &path) {
    const auto reader = axk::FileReader::open(path);
    EXPECT_TRUE(reader) << reader.error().message;
    return reader ? *reader : nullptr;
}

std::uint32_t only_volume_directory(const axk::MediaContainer &media) {
    const auto catalog = axk::build_object_catalog(media);
    EXPECT_TRUE(catalog) << catalog.error().message;
    if (!catalog || catalog->objects.empty() || !catalog->objects.front().placement)
        return 0U;
    return catalog->objects.front().placement->volume_directory.value;
}

} // namespace

TEST(MediaConversion, WritesOnePartitionAsIsoAndOneVolumeAsFloppyFromRetainedReader) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-media-conversion";
    const auto audio_path = root / "source.wav";
    const auto source_path = root / "source.hds";
    const auto iso_path = root / "partition.iso";
    const auto floppy_path = root / "volume.ima";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    axk::Waveform waveform;
    waveform.format = {1U, 2U, 44'100U};
    waveform.frame_count = 4U;
    waveform.pcm = {std::byte{}, std::byte{}, std::byte{0x34}, std::byte{0x12},
                    std::byte{}, std::byte{}, std::byte{0xcc}, std::byte{0xed}};
    ASSERT_TRUE(axk::write_wav_atomic(audio_path, waveform));
    const axk::HdsBuildManifest source_manifest{
        "1.0", 8U * 1024U * 1024U, {{"PARTITION ONE", {source_volume(audio_path)}}}};
    const auto source_written = axk::write_hds_image(source_manifest, source_path);
    ASSERT_TRUE(source_written) << source_written.error().message;
    const auto source_media = axk::open_media(source_path);
    ASSERT_TRUE(source_media) << source_media.error().message;
    const auto source_payloads = payloads(*source_media);
    ASSERT_EQ(source_payloads.size(), 5U);

    axk::MediaConversionRequest iso_request;
    iso_request.format = axk::MediaImageFormat::iso9660;
    iso_request.scope = axk::MediaConversionScope::partition;
    iso_request.iso_volume_id = "source partition";
    const auto iso_plan = axk::plan_media_conversion(open_reader(source_path), source_path, iso_request);
    ASSERT_TRUE(iso_plan) << iso_plan.error().message;
    EXPECT_TRUE(iso_plan->can_export);
    EXPECT_TRUE(iso_plan->issues.empty());
    EXPECT_EQ(iso_plan->object_count, 5U);
    ASSERT_EQ(iso_plan->volumes.size(), 1U);
    EXPECT_EQ(iso_plan->volumes.front().name, "Source Volume");
    EXPECT_EQ(iso_plan->volumes.front().raw_volume, "F001");
    EXPECT_GT(iso_plan->projected_output_bytes, iso_plan->payload_bytes);

    const auto iso_written = axk::write_media_conversion(open_reader(source_path), source_path, iso_request, iso_path);
    ASSERT_TRUE(iso_written) << iso_written.error().message;
    EXPECT_EQ(iso_written->size_bytes, iso_plan->projected_output_bytes);
    const auto iso_media = axk::open_media(iso_path);
    ASSERT_TRUE(iso_media) << iso_media.error().message;
    EXPECT_EQ(payloads(*iso_media), source_payloads);

    axk::MediaConversionRequest floppy_request;
    floppy_request.format = axk::MediaImageFormat::fat12_floppy;
    floppy_request.scope = axk::MediaConversionScope::volume;
    floppy_request.volume_directory_id = only_volume_directory(*source_media);
    const auto floppy_plan = axk::plan_media_conversion(open_reader(source_path), source_path, floppy_request);
    ASSERT_TRUE(floppy_plan) << floppy_plan.error().message;
    EXPECT_TRUE(floppy_plan->can_export);
    EXPECT_EQ(floppy_plan->projected_output_bytes, 1'474'560U);

    const auto floppy_written =
        axk::write_media_conversion(open_reader(source_path), source_path, floppy_request, floppy_path);
    ASSERT_TRUE(floppy_written) << floppy_written.error().message;
    const auto floppy_media = axk::open_media(floppy_path);
    ASSERT_TRUE(floppy_media) << floppy_media.error().message;
    EXPECT_EQ(payloads(*floppy_media), source_payloads);
    std::filesystem::remove_all(root, error);
}

TEST(MediaConversion, WritesMultipleIsoVolumesAndPackagesOversizedWaveDataAsAFloppyDiskSet) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-media-conversion-blockers";
    const auto audio_path = root / "source.wav";
    const auto source_path = root / "source.hds";
    const auto iso_path = root / "partition.iso";
    const auto floppy_path = root / "volume.zip";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    axk::Waveform waveform;
    waveform.format = {1U, 2U, 44'100U};
    waveform.frame_count = 800'000U;
    waveform.pcm.resize(static_cast<std::size_t>(waveform.frame_count) * 2U);
    ASSERT_TRUE(axk::write_wav_atomic(audio_path, waveform));
    const axk::HdsBuildManifest source_manifest{
        "1.0",
        16U * 1024U * 1024U,
        {{"PARTITION ONE", {source_volume(audio_path), source_volume(audio_path, "Second Volume")}}}};
    const auto source_written = axk::write_hds_image(source_manifest, source_path);
    ASSERT_TRUE(source_written) << source_written.error().message;
    const auto source_media = axk::open_media(source_path);
    ASSERT_TRUE(source_media) << source_media.error().message;

    axk::MediaConversionRequest iso_request;
    iso_request.format = axk::MediaImageFormat::iso9660;
    iso_request.scope = axk::MediaConversionScope::partition;
    const auto iso_plan = axk::plan_media_conversion(open_reader(source_path), source_path, iso_request);
    ASSERT_TRUE(iso_plan) << iso_plan.error().message;
    EXPECT_TRUE(iso_plan->can_export);
    EXPECT_EQ(iso_plan->volumes.size(), 2U);
    EXPECT_TRUE(iso_plan->issues.empty());

    const auto iso_written = axk::write_media_conversion(open_reader(source_path), source_path, iso_request, iso_path);
    ASSERT_TRUE(iso_written) << iso_written.error().message;
    const auto iso_media = axk::open_media(iso_path);
    ASSERT_TRUE(iso_media) << iso_media.error().message;
    EXPECT_EQ(payloads(*iso_media), payloads(*source_media));
    const auto iso_catalog = axk::build_object_catalog(*iso_media);
    ASSERT_TRUE(iso_catalog) << iso_catalog.error().message;
    EXPECT_EQ(std::ranges::count_if(iso_catalog->objects,
                                    [](const auto &object) {
                                        return object.placement && object.placement->volume_name == "Source Volume";
                                    }),
              5U);
    EXPECT_EQ(std::ranges::count_if(iso_catalog->objects,
                                    [](const auto &object) {
                                        return object.placement && object.placement->volume_name == "Second Volume";
                                    }),
              5U);

    axk::MediaConversionRequest floppy_request;
    floppy_request.format = axk::MediaImageFormat::fat12_floppy;
    floppy_request.scope = axk::MediaConversionScope::volume;
    floppy_request.volume_directory_id = only_volume_directory(*source_media);
    const auto floppy_plan = axk::plan_media_conversion(open_reader(source_path), source_path, floppy_request);
    ASSERT_TRUE(floppy_plan) << floppy_plan.error().message;
    EXPECT_TRUE(floppy_plan->can_export);
    EXPECT_EQ(floppy_plan->artifact_kind, axk::MediaConversionArtifactKind::floppy_disk_set);
    EXPECT_EQ(floppy_plan->output_extension, ".zip");
    EXPECT_EQ(floppy_plan->floppy_image_count, 2U);
    EXPECT_GT(floppy_plan->projected_output_bytes, 2U * 1'474'560U);
    const auto pending =
        std::ranges::find(floppy_plan->issues, std::string{"MEDIA_CONVERSION_MULTI_FLOPPY_HARDWARE_VALIDATION_PENDING"},
                          &axk::MediaConversionIssue::code);
    ASSERT_NE(pending, floppy_plan->issues.end());
    EXPECT_FALSE(pending->blocking);
    ASSERT_TRUE(pending->measurement);
    EXPECT_EQ(pending->measurement->required, 2U);
    EXPECT_EQ(pending->measurement->available, 32U);
    EXPECT_EQ(pending->measurement->unit, axk::MediaConversionIssueUnit::floppy_images);

    const auto floppy_written =
        axk::write_media_conversion(open_reader(source_path), source_path, floppy_request, floppy_path);
    ASSERT_TRUE(floppy_written) << floppy_written.error().message;
    EXPECT_EQ(floppy_written->artifact_kind, axk::MediaConversionArtifactKind::floppy_disk_set);
    EXPECT_EQ(floppy_written->floppy_image_count, 2U);
    auto archive_reader = axk::FileReader::open(floppy_path);
    ASSERT_TRUE(archive_reader) << archive_reader.error().message;
    std::vector<std::byte> archive_bytes(static_cast<std::size_t>((*archive_reader)->size()));
    ASSERT_TRUE((*archive_reader)->read_exact_at(0U, archive_bytes));
    const auto archive = axk::package_internal::read_archive(archive_bytes);
    ASSERT_TRUE(archive) << archive.error().message;
    ASSERT_EQ(archive->size(), 3U);
    EXPECT_EQ(archive->front().path, "manifest.json");
    std::vector<std::string> continuation_paths;
    for (std::size_t index = 1U; index < archive->size(); ++index) {
        EXPECT_EQ((*archive)[index].path, std::format("payloads/disk{:02}.ima", index));
        EXPECT_EQ((*archive)[index].bytes.size(), 1'474'560U);
        const auto disk =
            axk::FatImage::open(std::make_shared<axk::MemoryReader>((*archive)[index].bytes), (*archive)[index].path);
        ASSERT_TRUE(disk) << disk.error().message;
        for (const auto &file : disk->files()) {
            if (file.size < 0x42U)
                continue;
            const auto prefix = disk->read_file_prefix(file, 0x42U);
            ASSERT_TRUE(prefix) << prefix.error().message;
            const auto header = axk::decode_object_header(*prefix);
            if (header && header->type == axk::ObjectType::smpl)
                continuation_paths.push_back(file.path);
        }
    }
    ASSERT_EQ(continuation_paths.size(), 2U);
    EXPECT_EQ(continuation_paths.front(), continuation_paths.back());
    std::filesystem::remove_all(root, error);
}

TEST(MediaConversion, PreservesTheWaveDataFilenameAcrossContinuationDisks) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-media-conversion-continuation-name";
    const auto audio_path = root / "source.wav";
    const auto source_path = root / "source.hds";
    const auto output_path = root / "volume.zip";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    axk::Waveform waveform;
    waveform.format = {1U, 2U, 44'100U};
    waveform.frame_count = 800'000U;
    waveform.pcm.resize(static_cast<std::size_t>(waveform.frame_count) * 2U);
    ASSERT_TRUE(axk::write_wav_atomic(audio_path, waveform));
    const axk::HdsBuildManifest source_manifest{
        "1.0", 8U * 1024U * 1024U, {{"PARTITION ONE", {source_volume(audio_path)}}}};
    ASSERT_TRUE(axk::write_hds_image(source_manifest, source_path));
    const auto source = axk::open_media(source_path);
    ASSERT_TRUE(source) << source.error().message;
    const auto objects = source->objects();
    ASSERT_TRUE(objects) << objects.error().message;
    const auto program = std::ranges::find_if(
        *objects, [](const axk::MediaObject &object) { return object.decoded.header.type == axk::ObjectType::prog; });
    const auto wave_data = std::ranges::find_if(
        *objects, [](const axk::MediaObject &object) { return object.decoded.header.type == axk::ObjectType::smpl; });
    ASSERT_NE(program, objects->end());
    ASSERT_NE(wave_data, objects->end());

    axk::detail::PreparedMediaImage image;
    image.manifest.format = axk::MediaImageFormat::fat12_floppy;
    image.objects.emplace_back(program->decoded.header.type, program->decoded.header.name, program->raw_payload);
    image.objects.emplace_back(wave_data->decoded.header.type, wave_data->decoded.header.name, wave_data->raw_payload);
    const auto plan = axk::detail::plan_floppy_disk_set(image, "Continuation", {});
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_EQ(plan->disks.size(), 2U);
    const auto written = axk::detail::write_floppy_disk_set(image, *plan, output_path, false, {});
    ASSERT_TRUE(written) << written.error().message;

    const auto archive_reader = axk::FileReader::open(output_path);
    ASSERT_TRUE(archive_reader) << archive_reader.error().message;
    std::vector<std::byte> archive_bytes(static_cast<std::size_t>((*archive_reader)->size()));
    ASSERT_TRUE((*archive_reader)->read_exact_at(0U, archive_bytes));
    const auto archive = axk::package_internal::read_archive(archive_bytes);
    ASSERT_TRUE(archive) << archive.error().message;
    std::vector<std::string> continuation_paths;
    for (std::size_t index = 1U; index < archive->size(); ++index) {
        const auto disk =
            axk::FatImage::open(std::make_shared<axk::MemoryReader>((*archive)[index].bytes), (*archive)[index].path);
        ASSERT_TRUE(disk) << disk.error().message;
        for (const auto &file : disk->files()) {
            if (file.size < 0x42U)
                continue;
            const auto prefix = disk->read_file_prefix(file, 0x42U);
            ASSERT_TRUE(prefix) << prefix.error().message;
            const auto header = axk::decode_object_header(*prefix);
            if (header && header->type == axk::ObjectType::smpl)
                continuation_paths.push_back(file.path);
        }
    }
    ASSERT_EQ(continuation_paths.size(), 2U);
    EXPECT_EQ(continuation_paths.front(), continuation_paths.back());
    std::filesystem::remove_all(root, error);
}

TEST(MediaConversion, WritesMultiSectorProgramAndSampleDirectories) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-media-conversion-multisector";
    const auto audio_path = root / "source.wav";
    const auto source_path = root / "source.hds";
    const auto iso_path = root / "partition.iso";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    axk::Waveform waveform;
    waveform.format = {1U, 2U, 44'100U};
    waveform.frame_count = 4U;
    waveform.pcm.resize(8U);
    ASSERT_TRUE(axk::write_wav_atomic(audio_path, waveform));
    const axk::HdsBuildManifest source_manifest{
        "1.0", 16U * 1024U * 1024U, {{"PARTITION ONE", {dense_source_volume(audio_path)}}}};
    const auto source_written = axk::write_hds_image(source_manifest, source_path);
    ASSERT_TRUE(source_written) << source_written.error().message;
    const auto source_media = axk::open_media(source_path);
    ASSERT_TRUE(source_media) << source_media.error().message;

    axk::MediaConversionRequest request;
    request.format = axk::MediaImageFormat::iso9660;
    request.scope = axk::MediaConversionScope::partition;
    const auto plan = axk::plan_media_conversion(open_reader(source_path), source_path, request);
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_TRUE(plan->can_export);
    EXPECT_TRUE(plan->issues.empty());

    auto floppy_request = request;
    floppy_request.format = axk::MediaImageFormat::fat12_floppy;
    floppy_request.scope = axk::MediaConversionScope::volume;
    floppy_request.volume_directory_id = only_volume_directory(*source_media);
    const auto floppy_plan = axk::plan_media_conversion(open_reader(source_path), source_path, floppy_request);
    ASSERT_TRUE(floppy_plan) << floppy_plan.error().message;
    EXPECT_TRUE(floppy_plan->can_export);
    EXPECT_EQ(floppy_plan->artifact_kind, axk::MediaConversionArtifactKind::floppy_disk_set);
    EXPECT_EQ(floppy_plan->floppy_image_count, 2U);
    EXPECT_EQ(floppy_plan->output_extension, ".zip");

    const auto written = axk::write_media_conversion(open_reader(source_path), source_path, request, iso_path);
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(std::filesystem::file_size(iso_path), plan->projected_output_bytes);
    const auto iso = axk::IsoImage::open(iso_path);
    ASSERT_TRUE(iso) << iso.error().message;
    const auto program_directory =
        std::ranges::find(iso->files(), std::string{"46DEF120/F001/PROG"}, &axk::IsoFile::path);
    ASSERT_NE(program_directory, iso->files().end());
    EXPECT_TRUE(program_directory->is_directory);
    EXPECT_EQ(program_directory->size, 4'096U);
    const auto sample_directory =
        std::ranges::find(iso->files(), std::string{"46DEF120/F001/SBNK"}, &axk::IsoFile::path);
    ASSERT_NE(sample_directory, iso->files().end());
    EXPECT_TRUE(sample_directory->is_directory);
    EXPECT_EQ(sample_directory->size, 8'192U);

    const axk::MediaContainer iso_media{*iso};
    EXPECT_EQ(payloads(iso_media), payloads(*source_media));
    std::filesystem::remove_all(root, error);
}

TEST(MediaConversion, RetainsExactMissingProgramRowsAsOneNonblockingMediaWarning) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-media-conversion-disabled-program-row";
    const auto audio_path = root / "source.wav";
    const auto source_path = root / "source.hds";
    const auto iso_path = root / "partition.iso";
    const auto floppy_path = root / "volume.ima";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    axk::Waveform waveform;
    waveform.format = {1U, 2U, 44'100U};
    waveform.frame_count = 4U;
    waveform.pcm.resize(8U);
    ASSERT_TRUE(axk::write_wav_atomic(audio_path, waveform));
    const axk::HdsBuildManifest source_manifest{
        "1.0", 8U * 1024U * 1024U, {{"PARTITION ONE", {source_volume(audio_path)}}}};
    ASSERT_TRUE(axk::write_hds_image(source_manifest, source_path));
    make_program_assignment_target_missing_with_same_type_context(source_path);
    const auto source_media = axk::open_media(source_path);
    ASSERT_TRUE(source_media) << source_media.error().message;

    axk::MediaConversionRequest request;
    request.format = axk::MediaImageFormat::iso9660;
    request.scope = axk::MediaConversionScope::partition;
    const auto plan = axk::plan_media_conversion(open_reader(source_path), source_path, request);
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_TRUE(plan->can_export);
    ASSERT_EQ(plan->issues.size(), 1U);
    EXPECT_EQ(plan->issues.front().code, "MEDIA_CONVERSION_RETAINED_DISABLED_PROGRAM_ROWS");
    EXPECT_FALSE(plan->issues.front().blocking);

    const auto written = axk::write_media_conversion(open_reader(source_path), source_path, request, iso_path);
    ASSERT_TRUE(written) << written.error().message;
    const auto iso_media = axk::open_media(iso_path);
    ASSERT_TRUE(iso_media) << iso_media.error().message;
    EXPECT_EQ(payloads(*iso_media), payloads(*source_media));

    request.format = axk::MediaImageFormat::fat12_floppy;
    request.scope = axk::MediaConversionScope::volume;
    request.volume_directory_id = only_volume_directory(*source_media);
    const auto floppy_plan = axk::plan_media_conversion(open_reader(source_path), source_path, request);
    ASSERT_TRUE(floppy_plan) << floppy_plan.error().message;
    EXPECT_TRUE(floppy_plan->can_export);
    ASSERT_EQ(floppy_plan->issues.size(), 1U);
    EXPECT_EQ(floppy_plan->issues.front().code, "MEDIA_CONVERSION_RETAINED_DISABLED_PROGRAM_ROWS");
    EXPECT_FALSE(floppy_plan->issues.front().blocking);

    const auto floppy_written =
        axk::write_media_conversion(open_reader(source_path), source_path, request, floppy_path);
    ASSERT_TRUE(floppy_written) << floppy_written.error().message;
    const auto floppy_media = axk::open_media(floppy_path);
    ASSERT_TRUE(floppy_media) << floppy_media.error().message;
    EXPECT_EQ(payloads(*floppy_media), payloads(*source_media));
    std::filesystem::remove_all(root, error);
}

TEST(MediaConversion, PlansMultiSectorGroupDirectoryAndPathTables) {
    axk::detail::PreparedMediaImage image;
    image.manifest.schema_version = std::string{axk::build_manifest_schema_version};
    image.manifest.format = axk::MediaImageFormat::iso9660;
    image.manifest.iso_volume_id = "PATH_TABLE_TEST";
    for (std::size_t index = 1U; index <= 998U; ++index) {
        image.iso_volumes.push_back(
            {"46DEF120", "Many Volumes", std::format("F{:03}", index), std::format("Volume {:03}", index), {}});
    }

    const auto layout = axk::detail::plan_iso9660_layout(image);
    ASSERT_TRUE(layout) << layout.error().message;
    EXPECT_GT(layout->little_path_table.size(), 2'048U);
    EXPECT_EQ(layout->little_path_table.size(), layout->big_path_table.size());
    EXPECT_GT(layout->big_path_sector, layout->little_path_sector + 1U);
    const auto group = std::ranges::find(layout->nodes, std::string{"46DEF120"}, &axk::detail::Iso9660LayoutNode::name);
    ASSERT_NE(group, layout->nodes.end());
    EXPECT_GT(group->extent_size, 2'048U);
    EXPECT_EQ(layout->output_bytes, static_cast<std::uint64_t>(layout->sector_count) * 2'048U);
}

TEST(MediaConversion, RejectsAggregateIsoSectorOverflowWithoutAllocatingPayloads) {
    axk::detail::PreparedMediaImage image;
    image.manifest.schema_version = std::string{axk::build_manifest_schema_version};
    image.manifest.format = axk::MediaImageFormat::iso9660;
    image.manifest.iso_volume_id = "SECTOR_OVERFLOW";
    const auto payload = std::make_shared<SparseReader>(std::numeric_limits<std::uint32_t>::max());
    for (std::size_t volume_index = 1U; volume_index <= 3U; ++volume_index) {
        axk::detail::PreparedIsoVolume volume{"46DEF120",
                                              "Many Volumes",
                                              std::format("F{:03}", volume_index),
                                              std::format("Volume {:03}", volume_index),
                                              {}};
        for (std::size_t object_index = 1U; object_index <= 683U; ++object_index) {
            volume.objects.emplace_back(axk::ObjectType::unknown, std::format("Object {:03}", object_index), payload);
        }
        image.iso_volumes.push_back(std::move(volume));
    }

    const auto layout = axk::detail::plan_iso9660_layout(image);
    ASSERT_FALSE(layout);
    EXPECT_EQ(layout.error().code, axk::ErrorCode::unsupported_profile);
    EXPECT_EQ(layout.error().message, "ISO9660 sector count exceeds the 32-bit extent profile");
}

TEST(MediaConversion, PlansEveryRequiredFloppyBeforeTheThirtyTwoImageAdmissionLimitIsApplied) {
    axk::detail::PreparedMediaImage image;
    const auto payload = std::make_shared<SparseReader>(1'000'000U);
    for (std::size_t index = 1U; index <= 33U; ++index)
        image.objects.emplace_back(axk::ObjectType::prog, std::format("Program {:02}", index), payload);

    const auto plan = axk::detail::plan_floppy_disk_set(image, "Disk set", {});
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_EQ(plan->disks.size(), 33U);
    EXPECT_EQ(plan->disks.front().name, "DISK SET      01");
    EXPECT_EQ(plan->disks.back().name, "DISK SET      33");
}

TEST(MediaConversion, PropagatesCancellationWhilePlanningAFloppyDiskSet) {
    axk::detail::PreparedMediaImage image;
    image.objects.emplace_back(axk::ObjectType::prog, "Program", std::vector<std::byte>(512U));
    axk::CancellationSource cancellation;
    cancellation.cancel();

    const auto plan = axk::detail::plan_floppy_disk_set(image, "Disk set", cancellation.token());
    ASSERT_FALSE(plan);
    EXPECT_EQ(plan.error().code, axk::ErrorCode::operation_cancelled);
}
