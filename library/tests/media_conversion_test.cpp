#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/io.hpp"
#include "axklib/media.hpp"
#include "axklib/writer.hpp"

namespace {

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

TEST(MediaConversion, WritesMultipleIsoVolumesAndReportsFloppyCapacityBeforeWriting) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-media-conversion-blockers";
    const auto audio_path = root / "source.wav";
    const auto source_path = root / "source.hds";
    const auto iso_path = root / "partition.iso";
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
    EXPECT_FALSE(floppy_plan->can_export);
    const auto capacity = std::ranges::find(floppy_plan->issues, std::string{"MEDIA_CONVERSION_FLOPPY_CAPACITY"},
                                            &axk::MediaConversionIssue::code);
    ASSERT_NE(capacity, floppy_plan->issues.end());
    ASSERT_TRUE(capacity->required_bytes);
    ASSERT_TRUE(capacity->available_bytes);
    EXPECT_GT(*capacity->required_bytes, *capacity->available_bytes);
    std::filesystem::remove_all(root, error);
}
