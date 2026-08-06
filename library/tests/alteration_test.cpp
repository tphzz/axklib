#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iterator>
#include <ranges>
#include <set>

#include <gtest/gtest.h>

#include "../src/alteration_internal.hpp"
#include "axklib/alteration.hpp"
#include "axklib/alteration_transaction.hpp"
#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/package.hpp"
#include "axklib/relationship.hpp"
#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

namespace {

std::vector<char> bytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
}

axk::HdsBuildManifest source_manifest() {
    axk::HdsBuildManifest result{"1.0", 4U * 1024U * 1024U, {}};
    axk::VolumeSpec retained;
    retained.name = "Retained";
    axk::VolumeSpec removed;
    removed.name = "Removed";
    result.partitions.push_back({"hd1", {std::move(retained), std::move(removed)}});
    return result;
}

axk::HdsBuildManifest sample_source_manifest(const std::filesystem::path &audio_path) {
    axk::HdsBuildManifest result{"1.0", 4U * 1024U * 1024U, {}};
    axk::VolumeSpec volume;
    volume.name = "Samples";
    volume.waveforms.push_back({"wave", "Wave", audio_path, 60U, {}});
    axk::SampleSpec sample;
    sample.name = "Old Sample";
    sample.waveform_id = "wave";
    sample.root_key = 60U;
    sample.key_low = 12U;
    sample.key_high = 108U;
    sample.level = 99U;
    volume.samples.push_back(std::move(sample));
    result.partitions.push_back({"hd1", {std::move(volume)}});
    return result;
}

axk::HdsBuildManifest chain_source_manifest(const std::filesystem::path &audio_path) {
    auto result = sample_source_manifest(audio_path);
    auto &volume = result.partitions[0].volumes[0];
    volume.name = "Chain";
    volume.samples[0].name = "Banked Sample";
    auto direct = volume.samples[0];
    direct.name = "Direct";
    volume.samples.push_back(std::move(direct));
    volume.sample_banks.push_back({"Bank", {"Banked Sample"}});
    axk::ProgramSpec program;
    program.number = 33U;
    program.assignments = {{"SBAC", "Bank", 1U}, {"SBNK", "Direct", 2U}};
    volume.programs.push_back(std::move(program));
    return result;
}

axk::HdsBuildManifest wide_sample_bank_source_manifest(const std::filesystem::path &audio_path) {
    auto result = sample_source_manifest(audio_path);
    auto &volume = result.partitions[0].volumes[0];
    volume.name = "Wide Bank";
    volume.samples[0].name = "Member 1";
    for (std::size_t index = 2U; index <= 5U; ++index) {
        auto member = volume.samples[0];
        member.name = std::format("Member {}", index);
        volume.samples.push_back(std::move(member));
    }
    auto direct = volume.samples[0];
    direct.name = "Direct";
    volume.samples.push_back(std::move(direct));
    volume.sample_banks.push_back({"Group", {"Member 1", "Member 2", "Member 3"}});
    axk::ProgramSpec program;
    program.number = 33U;
    program.assignments = {{"SBAC", "Group", 1U}, {"SBNK", "Direct", 2U}};
    volume.programs.push_back(std::move(program));
    return result;
}

axk::Waveform test_waveform() {
    axk::Waveform result;
    result.format = {1U, 2U, 44100U};
    result.frame_count = 4U;
    result.pcm = {std::byte{0},    std::byte{0},    std::byte{0xe8}, std::byte{3},
                  std::byte{0x18}, std::byte{0xfc}, std::byte{0},    std::byte{0}};
    return result;
}

std::vector<std::byte> test_midi() {
    return {
        std::byte{'M'},  std::byte{'T'},  std::byte{'h'},  std::byte{'d'},  std::byte{0},   std::byte{0},
        std::byte{0},    std::byte{6},    std::byte{0},    std::byte{0},    std::byte{0},   std::byte{1},
        std::byte{0},    std::byte{96},   std::byte{'M'},  std::byte{'T'},  std::byte{'r'}, std::byte{'k'},
        std::byte{0},    std::byte{0},    std::byte{0},    std::byte{17},   std::byte{0},   std::byte{0x90},
        std::byte{60},   std::byte{100},  std::byte{48},   std::byte{0xf0}, std::byte{2},   std::byte{0x7d},
        std::byte{0xf7}, std::byte{48},   std::byte{0x80}, std::byte{60},   std::byte{0},   std::byte{0},
        std::byte{0xff}, std::byte{0x2f}, std::byte{0},
    };
}

void write_bytes(const std::filesystem::path &path, std::span<const std::byte> content) {
    std::ofstream output{path, std::ios::binary};
    ASSERT_TRUE(output);
    output.write(reinterpret_cast<const char *>(content.data()), static_cast<std::streamsize>(content.size()));
    ASSERT_TRUE(output);
}

class CancellingProgress final : public axk::ProgressSink {
  public:
    CancellingProgress(axk::CancellationSource &source, std::uint64_t cancel_after)
        : source_(source), cancel_after_(cancel_after) {}

    void report(const axk::Progress &progress) noexcept override {
        if (progress.phase == axk::ProgressPhase::allocating && progress.completed == cancel_after_) {
            source_.cancel();
        }
    }

  private:
    axk::CancellationSource &source_;
    std::uint64_t cancel_after_{};
};

void mark_cluster_used(const std::filesystem::path &path, const axk::Partition &partition, std::uint32_t cluster) {
    std::fstream image{path, std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(image);
    const auto byte_index = cluster / 8U;
    const auto mask = static_cast<unsigned char>(0x80U >> (cluster % 8U));
    const std::array offsets{
        (static_cast<std::uint64_t>(partition.start_sector) +
         static_cast<std::uint64_t>(partition.bitmap_cluster) * partition.sectors_per_cluster) *
                512U +
            byte_index,
        static_cast<std::uint64_t>(partition.start_sector) * 512U + 2048U + byte_index,
    };
    for (const auto offset : offsets) {
        image.seekg(static_cast<std::streamoff>(offset));
        char value{};
        image.read(&value, 1);
        ASSERT_TRUE(image);
        value = static_cast<char>(static_cast<unsigned char>(value) | mask);
        image.seekp(static_cast<std::streamoff>(offset));
        image.write(&value, 1);
        ASSERT_TRUE(image);
    }
}

void add_single_cluster_record(const std::filesystem::path &path, const axk::Partition &partition, axk::SfsId id,
                               std::uint32_t cluster) {
    std::array<char, 72> record{};
    const auto put_be16 = [&](std::size_t offset, std::uint16_t value) {
        record[offset] = static_cast<char>(value >> 8U);
        record[offset + 1U] = static_cast<char>(value);
    };
    const auto put_be32 = [&](std::size_t offset, std::uint32_t value) {
        record[offset] = static_cast<char>(value >> 24U);
        record[offset + 1U] = static_cast<char>(value >> 16U);
        record[offset + 2U] = static_cast<char>(value >> 8U);
        record[offset + 3U] = static_cast<char>(value);
    };
    put_be16(0U, 1U);
    put_be16(4U, 1U);
    put_be32(6U, 1U);
    put_be32(0x0aU, cluster);
    put_be32(0x0eU, 1U);
    put_be32(0x12U, 1U);
    std::fill(record.begin() + 0x3aU, record.begin() + 0x42U, static_cast<char>(0xff));
    record[0x42U] = static_cast<char>(0x9e);
    record[0x47U] = 1;

    constexpr std::uint64_t index_block_size = 1024U;
    constexpr std::uint64_t index_record_size = 72U;
    constexpr std::uint64_t records_per_index_block = 14U;
    const auto index_base =
        (static_cast<std::uint64_t>(partition.start_sector) +
         static_cast<std::uint64_t>(partition.directory_index_cluster) * partition.sectors_per_cluster) *
        512U;
    const auto record_offset = index_base + (id.value / records_per_index_block) * index_block_size +
                               (id.value % records_per_index_block) * index_record_size;
    std::fstream image{path, std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(image);
    image.seekp(static_cast<std::streamoff>(record_offset));
    image.write(record.data(), static_cast<std::streamsize>(record.size()));
    ASSERT_TRUE(image);
    mark_cluster_used(path, partition, cluster);
}

void mark_cluster_free(const std::filesystem::path &path, const axk::Partition &partition, std::uint32_t cluster) {
    std::fstream image{path, std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(image);
    const auto byte_index = cluster / 8U;
    const auto mask = static_cast<unsigned char>(0x80U >> (cluster % 8U));
    const std::array offsets{
        (static_cast<std::uint64_t>(partition.start_sector) +
         static_cast<std::uint64_t>(partition.bitmap_cluster) * partition.sectors_per_cluster) *
                512U +
            byte_index,
        static_cast<std::uint64_t>(partition.start_sector) * 512U + 2048U + byte_index,
    };
    for (const auto offset : offsets) {
        image.seekg(static_cast<std::streamoff>(offset));
        char value{};
        image.read(&value, 1);
        ASSERT_TRUE(image);
        value = static_cast<char>(static_cast<unsigned char>(value) & static_cast<unsigned char>(~mask));
        image.seekp(static_cast<std::streamoff>(offset));
        image.write(&value, 1);
        ASSERT_TRUE(image);
    }
}

void patch_record_byte(const std::filesystem::path &path, const axk::Partition &partition,
                       const axk::IndexRecord &record, std::size_t payload_offset, std::byte value) {
    ASSERT_EQ(record.extents.size(), 1U);
    const auto absolute =
        (static_cast<std::uint64_t>(partition.start_sector) +
         static_cast<std::uint64_t>(record.extents[0].cluster_offset) * partition.sectors_per_cluster) *
            512U +
        payload_offset;
    std::fstream image{path, std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(image);
    image.seekp(static_cast<std::streamoff>(absolute));
    image.put(static_cast<char>(value));
    ASSERT_TRUE(image);
}

void patch_record_be32(const std::filesystem::path &path, const axk::Partition &partition,
                       const axk::IndexRecord &record, std::size_t payload_offset, std::uint32_t value) {
    patch_record_byte(path, partition, record, payload_offset, static_cast<std::byte>((value >> 24U) & 0xffU));
    patch_record_byte(path, partition, record, payload_offset + 1U, static_cast<std::byte>((value >> 16U) & 0xffU));
    patch_record_byte(path, partition, record, payload_offset + 2U, static_cast<std::byte>((value >> 8U) & 0xffU));
    patch_record_byte(path, partition, record, payload_offset + 3U, static_cast<std::byte>(value & 0xffU));
}

void patch_index_be32(const std::filesystem::path &path, const axk::IndexRecord &record, std::size_t offset,
                      std::uint32_t value) {
    std::fstream image{path, std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(image);
    const std::array bytes{static_cast<char>((value >> 24U) & 0xffU), static_cast<char>((value >> 16U) & 0xffU),
                           static_cast<char>((value >> 8U) & 0xffU), static_cast<char>(value & 0xffU)};
    image.seekp(static_cast<std::streamoff>(record.record_offset.value + offset));
    image.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(image);
}

void patch_partition_header_be32(const std::filesystem::path &path, const axk::Partition &partition, std::size_t offset,
                                 std::uint32_t value) {
    std::fstream image{path, std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(image);
    const std::array encoded{static_cast<char>((value >> 24U) & 0xffU), static_cast<char>((value >> 16U) & 0xffU),
                             static_cast<char>((value >> 8U) & 0xffU), static_cast<char>(value & 0xffU)};
    for (const auto copy_offset : {std::uint64_t{0}, std::uint64_t{1024}}) {
        const auto absolute = static_cast<std::uint64_t>(partition.start_sector) * 512U + copy_offset + offset;
        image.seekp(static_cast<std::streamoff>(absolute));
        image.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        ASSERT_TRUE(image);
    }
}

void convert_to_parseable_1024_byte_sector_geometry(const std::filesystem::path &path,
                                                    const axk::Container &container) {
    ASSERT_EQ(container.superblock().sector_size_bytes, 512U);
    ASSERT_EQ(container.superblock().total_sector_count % 2U, 0U);
    ASSERT_EQ(container.partitions().size(), 1U);
    const auto &partition = container.partitions().front();
    ASSERT_EQ(partition.sectors_per_cluster, 2U);
    ASSERT_EQ(partition.start_sector % 2U, 1U);
    const auto aligned_start = partition.start_sector - 1U;
    const auto partition_source = static_cast<std::size_t>(partition.start_sector) * 512U;
    const auto partition_destination = static_cast<std::size_t>(aligned_start) * 512U;
    const auto partition_bytes = static_cast<std::size_t>(partition.sector_count) * 512U;
    auto image = bytes(path);
    ASSERT_LE(partition_source + partition_bytes, image.size());
    std::memmove(image.data() + partition_destination, image.data() + partition_source, partition_bytes);
    std::fill(image.begin() + static_cast<std::ptrdiff_t>(partition_destination + partition_bytes), image.end(), '\0');

    const auto put_be32 = [&](std::size_t offset, std::uint32_t value) {
        ASSERT_LE(offset + 4U, image.size());
        image[offset] = static_cast<char>((value >> 24U) & 0xffU);
        image[offset + 1U] = static_cast<char>((value >> 16U) & 0xffU);
        image[offset + 2U] = static_cast<char>((value >> 8U) & 0xffU);
        image[offset + 3U] = static_cast<char>(value & 0xffU);
    };
    put_be32(0x09cU, 1024U);
    put_be32(0x0a0U, container.superblock().total_sector_count / 2U);
    const auto table_offset = 0x0a8U + static_cast<std::size_t>(partition.index.value) * 8U;
    put_be32(table_offset, aligned_start / 2U);
    put_be32(table_offset + 4U, (container.superblock().total_sector_count - aligned_start) / 2U);
    put_be32(partition_destination + 0x94U, 1U);
    put_be32(partition_destination + 1024U + 0x94U, 1U);
    std::copy_n(image.begin(), 512U, image.begin() + 1024U);

    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(output);
    output.write(image.data(), static_cast<std::streamsize>(image.size()));
    ASSERT_TRUE(output);
}

void patch_record_name(const std::filesystem::path &path, const axk::Partition &partition,
                       const axk::IndexRecord &record, std::size_t payload_offset, std::string_view name) {
    ASSERT_LE(name.size(), 16U);
    for (std::size_t index = 0; index < 16U; ++index) {
        patch_record_byte(path, partition, record, payload_offset + index,
                          index < name.size() ? static_cast<std::byte>(name[index]) : std::byte{' '});
    }
}

} // namespace

TEST(Alteration, InsertsRenamesAndDeletesSequenceObjects) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-sequence";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    const auto source = root / "source.hds";
    const auto inserted = root / "inserted.hds";
    const auto renamed = root / "renamed.hds";
    const auto removed = root / "removed.hds";
    const auto midi = root / "source.mid";
    write_bytes(midi, test_midi());

    const auto written = axk::write_hds_image(source_manifest(), source);
    ASSERT_TRUE(written) << written.error().message;

    const axk::AlterationManifest insert_manifest{
        "1.0",
        {{"insert-sequence",
          axk::InsertSequenceOperation{
              axk::PartitionIndex{0U}, "Retained", {"Original", midi, axk::SequenceSystemExclusivePolicy::preserve}}}},
    };
    const auto inserted_result = axk::alter_hds(source, insert_manifest, inserted);
    ASSERT_TRUE(inserted_result) << inserted_result.error().message;
    auto inserted_container = axk::open_image(inserted);
    ASSERT_TRUE(inserted_container) << inserted_container.error().message;
    auto inserted_catalog = axk::build_object_catalog(*inserted_container);
    ASSERT_TRUE(inserted_catalog) << inserted_catalog.error().message;
    const auto sequence = std::ranges::find_if(inserted_catalog->objects, [](const auto &object) {
        return object.object.header.type == axk::ObjectType::sequ && object.object.header.name == "Original";
    });
    ASSERT_NE(sequence, inserted_catalog->objects.end());
    const auto *decoded = std::get_if<axk::CurrentSequence>(&sequence->object.payload);
    ASSERT_NE(decoded, nullptr);
    EXPECT_EQ(decoded->event_count, 4U);
    EXPECT_EQ(decoded->events[1].tick, 48U);
    EXPECT_EQ(decoded->events[1].kind, axk::SequenceEventKind::system_exclusive);
    EXPECT_EQ(decoded->events[1].message, (std::vector<std::byte>{std::byte{0xf0}, std::byte{0x7d}, std::byte{0xf7}}));
    EXPECT_EQ(decoded->events[2].tick, 96U);

    const axk::AlterationManifest rename_manifest{
        "1.0",
        {{"rename-sequence", axk::RenameSequenceOperation{axk::PartitionIndex{0U}, "Retained", "Original", "Renamed"}}},
    };
    const auto renamed_result = axk::alter_hds(inserted, rename_manifest, renamed);
    ASSERT_TRUE(renamed_result) << renamed_result.error().message;
    auto renamed_container = axk::open_image(renamed);
    ASSERT_TRUE(renamed_container) << renamed_container.error().message;
    auto renamed_catalog = axk::build_object_catalog(*renamed_container);
    ASSERT_TRUE(renamed_catalog) << renamed_catalog.error().message;
    const auto renamed_sequence = std::ranges::find_if(renamed_catalog->objects, [](const auto &object) {
        return object.object.header.type == axk::ObjectType::sequ && object.object.header.name == "Renamed";
    });
    ASSERT_NE(renamed_sequence, renamed_catalog->objects.end());
    const auto *renamed_decoded = std::get_if<axk::CurrentSequence>(&renamed_sequence->object.payload);
    ASSERT_NE(renamed_decoded, nullptr);
    EXPECT_EQ(renamed_decoded->raw_payload[0x54U], std::byte{'O'});

    const axk::AlterationManifest delete_manifest{
        "1.0",
        {{"delete-sequence", axk::DeleteSequenceOperation{axk::PartitionIndex{0U}, "Retained", "Renamed"}}},
    };
    const auto removed_result = axk::alter_hds(renamed, delete_manifest, removed);
    ASSERT_TRUE(removed_result) << removed_result.error().message;
    auto removed_container = axk::open_image(removed);
    ASSERT_TRUE(removed_container) << removed_container.error().message;
    auto removed_catalog = axk::build_object_catalog(*removed_container);
    ASSERT_TRUE(removed_catalog) << removed_catalog.error().message;
    EXPECT_EQ(std::ranges::count(removed_catalog->objects, axk::ObjectType::sequ,
                                 [](const auto &object) { return object.object.header.type; }),
              0U);
    std::filesystem::remove_all(root, error);
}

TEST(AlterationManifest, RequiresStrictOrderedBackwardReferences) {
    constexpr std::string_view valid = R"({
    "schema_version":"1.0","operations":[
      {"id":"first","type":"delete_volume","partition_index":0,"volume_name":"Removed"},
      {"id":"second","type":"delete_volume","partition_index":{"operation_ref":"first"},"volume_name":"Retained"}
    ]})";
    const auto parsed = axk::parse_alteration_manifest(valid);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->operations.size(), 2U);
    EXPECT_FALSE(axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"first","type":"delete_volume","partition_index":{"operation_ref":"later"},"volume_name":"Removed"},
      {"id":"later","type":"delete_volume","partition_index":0,"volume_name":"Retained"}
    ]})"));
}

TEST(AlterationManifest, RejectsInvalidTypedManifestBeforeOpeningTheSourceImage) {
    axk::AlterationManifest manifest{
        "1.0",
        {
            {"duplicate", axk::DeleteVolumeOperation{axk::PartitionIndex{0U}, "Removed"}},
            {"duplicate", axk::DeleteVolumeOperation{axk::PartitionIndex{0U}, "Retained"}},
        },
    };
    const auto missing = std::filesystem::temp_directory_path() / "axklib-missing-alteration-source.hds";
    const auto output = std::filesystem::temp_directory_path() / "axklib-unused-alteration-output.hds";

    const auto altered = axk::alter_hds(missing, manifest, output);

    ASSERT_FALSE(altered);
    EXPECT_EQ(altered.error().code, axk::ErrorCode::transaction_rejected);
    EXPECT_EQ(altered.error().message, "duplicate operation id");

    axk::SampleSpec invalid_sample;
    invalid_sample.name = "Sample";
    invalid_sample.waveform_id = "Wave Data";
    invalid_sample.interleaved_audio_path = "audio.wav";
    manifest.operations = {
        {"insert", axk::InsertSampleOperation{axk::PartitionIndex{0U}, "Volume", std::move(invalid_sample)}},
    };

    const auto invalid_sample_result = axk::alter_hds(missing, manifest, output);

    ASSERT_FALSE(invalid_sample_result);
    EXPECT_EQ(invalid_sample_result.error().code, axk::ErrorCode::transaction_rejected);
    EXPECT_EQ(invalid_sample_result.error().message,
              "inserted Sample must reference existing Wave Data without authored-audio fields");
}

TEST(AlterationManifestTemplate, EmitsParseableStarterAndPublishesAtomically) {
    const auto serialized = axk::serialize_alteration_manifest_template();
    ASSERT_TRUE(serialized) << serialized.error().message;
    const auto parsed = axk::parse_alteration_manifest(*serialized);
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed->schema_version, axk::alteration_manifest_schema_version);
    ASSERT_EQ(parsed->operations.size(), 1U);
    EXPECT_EQ(axk::operation_type_name(parsed->operations.front().data), "rename_waveform");

    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-manifest-template-test";
    const auto path = root / "nested" / "transaction.json";
    std::error_code error;
    std::filesystem::remove_all(root, error);

    ASSERT_TRUE(axk::write_alteration_manifest_template(path));
    EXPECT_FALSE(axk::write_alteration_manifest_template(path));
    ASSERT_TRUE(axk::write_alteration_manifest_template(path, true));
    ASSERT_TRUE(axk::load_alteration_manifest(path));

    std::filesystem::remove_all(root, error);
}

TEST(AlterationManifest, ParsesStrictSampleOperations) {
    const auto parsed = axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"delete","type":"delete_sbnk","partition_index":0,
       "volume_name":"Samples","sample_name":"Old Sample"},
      {"id":"insert","type":"insert_sbnk","partition_index":{"operation_ref":"delete"},
       "volume_name":"Samples","sample":{"name":"New Sample","waveform_name":"Wave",
       "root_key":64,"fine_tune_cents":-12,"key_low":10,"key_high":100,
       "velocity_low":20,"velocity_high":110,"loop_mode":1,"loop_start_frame":17,
       "loop_length_frames":335}}
    ]})");
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto *insert = std::get_if<axk::InsertSampleOperation>(&parsed->operations[1].data);
    ASSERT_NE(insert, nullptr);
    EXPECT_EQ(insert->sample.level, 100U);
    EXPECT_EQ(insert->sample.fine_tune_cents, -12);
    EXPECT_EQ(insert->sample.velocity_low, 20U);
    EXPECT_EQ(insert->sample.velocity_high, 110U);
    EXPECT_EQ(insert->sample.loop_mode, axk::AudioSamplerLoopMode::forward_loop);
    EXPECT_EQ(insert->sample.loop_start_frame, 17U);
    EXPECT_EQ(insert->sample.loop_length_frames, 335U);
    EXPECT_FALSE(axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"insert","type":"insert_sbnk","partition_index":0,
       "volume_name":"Samples","sample":{"name":"New Sample","waveform_name":"Wave",
       "root_key":64,"key_low":100,"key_high":10}}
    ]})"));
}

TEST(AlterationManifest, ParsesStrictWaveformSamplerMetadataAndRejectsInvalidLoopWindows) {
    const auto parsed = axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"wave","type":"insert_waveform","partition_index":0,"volume_name":"Samples",
       "audio":{"path":"mapped.wav","waveform_names":["Mapped L"],"root_key":64,
       "fine_tune_cents":25,"loop_mode":1,"loop_start_frame":17,"loop_length_frames":335}}
    ]})");
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto *insert = std::get_if<axk::InsertWaveformOperation>(&parsed->operations.front().data);
    ASSERT_NE(insert, nullptr);
    EXPECT_EQ(insert->waveform.fine_tune_cents, 25);
    EXPECT_EQ(insert->waveform.loop_mode, axk::AudioSamplerLoopMode::forward_loop);
    EXPECT_EQ(insert->waveform.loop_start_frame, 17U);
    EXPECT_EQ(insert->waveform.loop_length_frames, 335U);

    EXPECT_FALSE(axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"wave","type":"insert_waveform","partition_index":0,"volume_name":"Samples",
       "audio":{"path":"mapped.wav","waveform_names":["Mapped L"],"root_key":64,
       "fine_tune_cents":25,"loop_mode":1,"loop_start_frame":400,"loop_length_frames":0}}
    ]})"));
}

TEST(AlterationManifest, RejectsObsoleteSampleAndSampleBankFields) {
    EXPECT_FALSE(axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"sample","type":"insert_sbnk","partition_index":0,
       "volume_name":"Volume","sample_bank":{"name":"Sample","waveform_name":"Wave",
       "root_key":60,"key_low":0,"key_high":127}},
      {"id":"bank","type":"insert_sbac","partition_index":0,
       "volume_name":"Volume","sample_bank_group":{"name":"Bank","member_sample_banks":["Sample"]}}
    ]})"));

    const auto rejected = axk::parse_alteration_manifest(R"({
    "schema_version":"1.1","operations":[
      {"id":"delete","type":"delete_volume","partition_index":0,"volume_name":"Volume"}
    ]})");
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().message, "manifest schema version must be 1.0");
}

TEST(AlterationManifest, ParsesLanguageNeutralFixtureIntoTypedVariants) {
    const auto path =
        std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/manifests/alteration/all-operations.json";
    const auto parsed = axk::load_alteration_manifest(path);
    ASSERT_TRUE(parsed) << parsed.error().message;
    constexpr std::array expected{
        std::string_view{"delete_volume"},    std::string_view{"insert_volume"},   std::string_view{"delete_sbnk"},
        std::string_view{"insert_sbnk"},      std::string_view{"insert_waveform"}, std::string_view{"delete_waveform"},
        std::string_view{"rename_waveform"},  std::string_view{"rename_sbnk"},     std::string_view{"delete_sbac"},
        std::string_view{"insert_sbac"},      std::string_view{"rename_sbac"},     std::string_view{"delete_program"},
        std::string_view{"insert_program"},   std::string_view{"rename_program"},  std::string_view{"delete_sequence"},
        std::string_view{"insert_sequence"},  std::string_view{"rename_sequence"}, std::string_view{"rename_volume"},
        std::string_view{"rename_partition"},
    };
    ASSERT_EQ(parsed->operations.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(axk::operation_type_name(parsed->operations[index].data), expected[index]);
        EXPECT_EQ(parsed->operations[index].data.index(), index);
    }
    const auto *deleted = std::get_if<axk::DeleteProgramOperation>(&parsed->operations[11].data);
    ASSERT_NE(deleted, nullptr);
    EXPECT_EQ(deleted->program_number, 128U);
}

TEST(AlterationManifest, ParsesStrictProgramRename) {
    const auto parsed = axk::parse_alteration_manifest(R"({
      "schema_version":"1.0","operations":[
        {"id":"rename","type":"rename_program","partition_index":0,
         "volume_name":"Programs","program_number":128,"new_program_name":"New Name"}
      ]})");
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto *rename = std::get_if<axk::RenameProgramOperation>(&parsed->operations.front().data);
    ASSERT_NE(rename, nullptr);
    EXPECT_EQ(rename->program_number, 128U);
    EXPECT_EQ(rename->new_program_name, "New Name");

    for (const auto *name : {"", "123456789", " Leading", "Trailing "}) {
        const auto rejected = axk::parse_alteration_manifest(std::format(
            R"({{"schema_version":"1.0","operations":[{{"id":"rename","type":"rename_program",)"
            R"("partition_index":0,"volume_name":"Programs","program_number":1,"new_program_name":"{}"}}]}})",
            name));
        EXPECT_FALSE(rejected) << name;
    }
}

TEST(AlterationManifest, RequiresAnExplicitSequenceSystemExclusivePolicy) {
    const auto parsed = axk::parse_alteration_manifest(R"({
      "schema_version":"1.0","operations":[
        {"id":"insert","type":"insert_sequence","partition_index":0,"volume_name":"Songs",
         "sequence":{"name":"Pattern","midi_path":"pattern.mid","system_exclusive_policy":"exclude"}}
      ]})");
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto *insert = std::get_if<axk::InsertSequenceOperation>(&parsed->operations.front().data);
    ASSERT_NE(insert, nullptr);
    EXPECT_EQ(insert->sequence.system_exclusive_policy, axk::SequenceSystemExclusivePolicy::exclude);

    EXPECT_FALSE(axk::parse_alteration_manifest(R"({
      "schema_version":"1.0","operations":[
        {"id":"insert","type":"insert_sequence","partition_index":0,"volume_name":"Songs",
         "sequence":{"name":"Pattern","midi_path":"pattern.mid"}}
      ]})"));
    EXPECT_FALSE(axk::parse_alteration_manifest(R"({
      "schema_version":"1.0","operations":[
        {"id":"insert","type":"insert_sequence","partition_index":0,"volume_name":"Songs",
         "sequence":{"name":"Pattern","midi_path":"pattern.mid","system_exclusive_policy":"drop"}}
      ]})"));
}

TEST(AlterationManifest, ParsesStrictVolumeRename) {
    const auto parsed = axk::parse_alteration_manifest(R"({
      "schema_version":"1.0","operations":[
        {"id":"rename","type":"rename_volume","partition_index":0,
         "volume_name":"Retained","new_volume_name":"Renamed"}
      ]})");
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto *rename = std::get_if<axk::RenameVolumeOperation>(&parsed->operations.front().data);
    ASSERT_NE(rename, nullptr);
    EXPECT_EQ(rename->volume_name, "Retained");
    EXPECT_EQ(rename->new_volume_name, "Renamed");

    EXPECT_FALSE(axk::parse_alteration_manifest(R"({
      "schema_version":"1.0","operations":[
        {"id":"rename","type":"rename_volume","partition_index":0,
         "volume_name":"Retained","new_volume_name":"Renamed","extra":true}
      ]})"));
}

TEST(AlterationManifest, ParsesStrictPartitionRename) {
    const auto parsed = axk::parse_alteration_manifest(R"({
      "schema_version":"1.0","operations":[
        {"id":"rename","type":"rename_partition","partition_index":1,
         "partition_name":"PARTITION 2","new_partition_name":"Samples"}
      ]})");
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto *rename = std::get_if<axk::RenamePartitionOperation>(&parsed->operations.front().data);
    ASSERT_NE(rename, nullptr);
    EXPECT_EQ(rename->partition_name, "PARTITION 2");
    EXPECT_EQ(rename->new_partition_name, "Samples");

    EXPECT_FALSE(axk::parse_alteration_manifest(R"({
      "schema_version":"1.0","operations":[
        {"id":"rename","type":"rename_partition","partition_index":1,
         "partition_name":"PARTITION 2","new_partition_name":"Samples","extra":true}
      ]})"));
    EXPECT_FALSE(axk::parse_alteration_manifest(R"({
      "schema_version":"1.0","operations":[
        {"id":"rename","type":"rename_partition","partition_index":1,
         "partition_name":"PARTITION 2","new_partition_name":"PARTITION 2"}
      ]})"));
}

TEST(Alteration, DeleteVolumeDryRunMatchesApplyAndPreservesSource) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-test";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_hds_image(source_manifest(), source));
    const auto source_before = bytes(source);
    const auto manifest = axk::parse_alteration_manifest(
        R"({"schema_version":"1.0","operations":[{"id":"remove","type":"delete_volume","partition_index":0,"volume_name":"Removed"}]})");
    ASSERT_TRUE(manifest);
    const auto inspected = axk::inspect_hds_alteration(source, *manifest);
    ASSERT_TRUE(inspected);
    ASSERT_EQ(inspected->operations.size(), 1U);
    EXPECT_EQ(inspected->operations[0].freed_clusters, 12U);
    const auto applied = axk::alter_hds(source, *manifest, output);
    ASSERT_TRUE(applied);
    EXPECT_EQ(applied->operations[0].removed_sfs_ids, inspected->operations[0].removed_sfs_ids);
    EXPECT_EQ(bytes(source), source_before);
    const auto reopened = axk::open_image(output);
    ASSERT_TRUE(reopened);
    const auto &root_record =
        *std::ranges::find(reopened->partitions()[0].records, axk::SfsId{1}, &axk::IndexRecord::sfs_id);
    EXPECT_TRUE(
        std::ranges::any_of(root_record.directory_entries, [](const auto &entry) { return entry.name == "Retained"; }));
    EXPECT_FALSE(
        std::ranges::any_of(root_record.directory_entries, [](const auto &entry) { return entry.name == "Removed"; }));
    EXPECT_FALSE(axk::alter_hds(source, *manifest, output));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, RejectsParseableNon512ByteSectorGeometryBeforePlanning) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-sector-profile";
    const auto source = root / "source.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_hds_image(source_manifest(), source));
    const auto original = axk::open_image(source);
    ASSERT_TRUE(original) << original.error().message;
    convert_to_parseable_1024_byte_sector_geometry(source, *original);
    const auto converted = axk::open_image(source);
    ASSERT_TRUE(converted) << converted.error().message;
    EXPECT_EQ(converted->superblock().sector_size_bytes, 1024U);
    const auto manifest = axk::parse_alteration_manifest(
        R"({"schema_version":"1.0","operations":[{"id":"rename","type":"rename_volume","partition_index":0,"volume_name":"Retained","new_volume_name":"Renamed"}]})");
    ASSERT_TRUE(manifest);

    const auto inspected = axk::inspect_hds_alteration(source, *manifest);
    ASSERT_FALSE(inspected);
    EXPECT_NE(inspected.error().message.find("512-byte alteration profile"), std::string::npos);
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, PreparedPatchesReproducePublishedMetadataAlteration) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-patches";
    const auto source = root / "source.hds";
    const auto expected = root / "expected.hds";
    const auto patched = root / "patched.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_hds_image(source_manifest(), source));
    const auto manifest = axk::parse_alteration_manifest(
        R"({"schema_version":"1.0","operations":[{"id":"rename","type":"rename_volume","partition_index":0,"volume_name":"Retained","new_volume_name":"Renamed"}]})");
    ASSERT_TRUE(manifest) << manifest.error().message;
    const auto reader = axk::FileReader::open(source);
    ASSERT_TRUE(reader) << reader.error().message;
    const auto prepared = axk::detail::prepare_hds_alteration(*reader, source, *manifest);
    ASSERT_TRUE(prepared) << prepared.error().message;
    ASSERT_FALSE(prepared->patches.empty());
    ASSERT_TRUE(axk::alter_hds(source, *manifest, expected));

    std::filesystem::copy_file(source, patched);
    std::fstream output{patched, std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(output);
    std::uint64_t previous_end{};
    for (const auto &patch : prepared->patches) {
        EXPECT_GE(patch.offset, previous_end);
        EXPECT_EQ(patch.original.size(), patch.replacement.size());
        output.seekp(static_cast<std::streamoff>(patch.offset));
        output.write(reinterpret_cast<const char *>(patch.replacement.data()),
                     static_cast<std::streamsize>(patch.replacement.size()));
        ASSERT_TRUE(output);
        previous_end = patch.offset + patch.replacement.size();
    }
    output.close();
    EXPECT_EQ(bytes(patched), bytes(expected));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, InsertsFirstVolumeIntoEmptyPartition) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-insert-first-volume";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    axk::HdsBuildManifest source_spec{"1.0", 4U * 1024U * 1024U, {}};
    source_spec.partitions.push_back({"hd1", {}});
    ASSERT_TRUE(axk::write_hds_image(source_spec, source));

    const auto manifest = axk::parse_alteration_manifest(R"({
      "schema_version":"1.0","operations":[
        {"id":"insert","type":"insert_volume","partition_index":0,
         "volume":{"name":"First Volume","waveforms":[],"samples":[]}}
      ]})");
    ASSERT_TRUE(manifest) << manifest.error().message;
    const auto applied = axk::alter_hds(source, *manifest, output);
    ASSERT_TRUE(applied) << applied.error().message;
    ASSERT_EQ(applied->operations.size(), 1U);
    EXPECT_FALSE(applied->operations.front().inserted_sfs_ids.empty());

    const auto reopened = axk::open_image(output);
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto &root_record =
        *std::ranges::find(reopened->partitions().front().records, axk::SfsId{1}, &axk::IndexRecord::sfs_id);
    const auto inserted = std::ranges::find(root_record.directory_entries, "First Volume", &axk::DirectoryEntry::name);
    ASSERT_NE(inserted, root_record.directory_entries.end());
    EXPECT_TRUE(std::ranges::any_of(applied->operations.front().inserted_sfs_ids,
                                    [&](const auto id) { return id.value == inserted->link_id.value; }));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, RenameVolumePreservesClosureAllocationAndExactPcm) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-rename-volume";
    const auto audio = root / "tone.wav";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_wav_atomic(audio, test_waveform()));
    ASSERT_TRUE(axk::write_hds_image(chain_source_manifest(audio), source));
    const auto before = axk::open_image(source);
    ASSERT_TRUE(before) << before.error().message;
    const auto &before_partition = before->partitions().front();
    const auto &before_root = *std::ranges::find(before_partition.records, axk::SfsId{1}, &axk::IndexRecord::sfs_id);
    const auto before_entry = std::ranges::find(before_root.directory_entries, "Chain", &axk::DirectoryEntry::name);
    ASSERT_NE(before_entry, before_root.directory_entries.end());
    const auto before_catalog = axk::build_object_catalog(*before);
    ASSERT_TRUE(before_catalog) << before_catalog.error().message;
    const auto before_wave = std::ranges::find_if(
        before_catalog->objects, [](const auto &object) { return object.object.header.type == axk::ObjectType::smpl; });
    ASSERT_NE(before_wave, before_catalog->objects.end());
    const auto before_pcm = axk::decode_waveform(*before, *before_wave);
    ASSERT_TRUE(before_pcm) << before_pcm.error().message;

    const auto manifest = axk::parse_alteration_manifest(R"({
      "schema_version":"1.0","operations":[
        {"id":"rename","type":"rename_volume","partition_index":0,
         "volume_name":"Chain","new_volume_name":"Renamed"}
      ]})");
    ASSERT_TRUE(manifest) << manifest.error().message;
    const auto applied = axk::alter_hds(source, *manifest, output);
    ASSERT_TRUE(applied) << applied.error().message;
    ASSERT_EQ(applied->operations.size(), 1U);
    EXPECT_TRUE(applied->operations.front().removed_sfs_ids.empty());
    EXPECT_TRUE(applied->operations.front().inserted_sfs_ids.empty());
    EXPECT_EQ(applied->operations.front().freed_clusters, 0U);
    EXPECT_EQ(applied->operations.front().allocated_clusters, 0U);

    const auto after = axk::open_image(output);
    ASSERT_TRUE(after) << after.error().message;
    const auto &after_partition = after->partitions().front();
    EXPECT_EQ(after_partition.allocation.stored_used_cluster_count,
              before_partition.allocation.stored_used_cluster_count);
    EXPECT_EQ(after_partition.allocation.reconstructed_used_cluster_count,
              before_partition.allocation.reconstructed_used_cluster_count);
    EXPECT_TRUE(before_partition.allocation.stored_not_reconstructed.empty());
    EXPECT_TRUE(after_partition.allocation.stored_not_reconstructed.empty());
    EXPECT_TRUE(before_partition.allocation.reconstructed_not_stored.empty());
    EXPECT_TRUE(after_partition.allocation.reconstructed_not_stored.empty());
    const auto &after_root = *std::ranges::find(after_partition.records, axk::SfsId{1}, &axk::IndexRecord::sfs_id);
    EXPECT_EQ(std::ranges::count(after_root.directory_entries, "Chain", &axk::DirectoryEntry::name), 0U);
    const auto after_entry = std::ranges::find(after_root.directory_entries, "Renamed", &axk::DirectoryEntry::name);
    ASSERT_NE(after_entry, after_root.directory_entries.end());
    EXPECT_EQ(after_entry->link_id, before_entry->link_id);
    const auto after_catalog = axk::build_object_catalog(*after);
    ASSERT_TRUE(after_catalog) << after_catalog.error().message;
    ASSERT_EQ(after_catalog->objects.size(), before_catalog->objects.size());
    for (const auto &object : before_catalog->objects) {
        const auto match = std::ranges::find(after_catalog->objects, object.sfs_id, &axk::ObjectSnapshot::sfs_id);
        ASSERT_NE(match, after_catalog->objects.end());
        EXPECT_EQ(match->object.header.type, object.object.header.type);
        EXPECT_EQ(match->object.header.name, object.object.header.name);
    }
    const auto after_wave =
        std::ranges::find(after_catalog->objects, before_wave->sfs_id, &axk::ObjectSnapshot::sfs_id);
    ASSERT_NE(after_wave, after_catalog->objects.end());
    const auto after_pcm = axk::decode_waveform(*after, *after_wave);
    ASSERT_TRUE(after_pcm) << after_pcm.error().message;
    EXPECT_EQ(after_pcm->pcm, before_pcm->pcm);

    const auto duplicate = axk::parse_alteration_manifest(R"({
      "schema_version":"1.0","operations":[
        {"id":"rename","type":"rename_volume","partition_index":0,
         "volume_name":"Chain","new_volume_name":"Chain"}
      ]})");
    EXPECT_FALSE(duplicate);
    const auto too_long = axk::parse_alteration_manifest(R"({
      "schema_version":"1.0","operations":[
        {"id":"rename","type":"rename_volume","partition_index":0,
         "volume_name":"Chain","new_volume_name":"This name is too long"}
      ]})");
    EXPECT_FALSE(too_long);
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, RenameProgramChangesOnlyTheSamplerVisibleDisplayName) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-rename-program";
    const auto audio = root / "tone.wav";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_wav_atomic(audio, test_waveform()));
    ASSERT_TRUE(axk::write_hds_image(chain_source_manifest(audio), source));

    const auto before = axk::open_image(source);
    ASSERT_TRUE(before) << before.error().message;
    const auto before_catalog = axk::build_object_catalog(*before);
    ASSERT_TRUE(before_catalog) << before_catalog.error().message;
    const auto before_program = std::ranges::find_if(before_catalog->objects, [](const auto &object) {
        return object.object.header.type == axk::ObjectType::prog && object.object.header.name == "033";
    });
    ASSERT_NE(before_program, before_catalog->objects.end());
    ASSERT_GE(before_program->raw_payload.size(), 0x80U);
    const auto before_payload = before_program->raw_payload;

    const auto manifest = axk::parse_alteration_manifest(R"({
      "schema_version":"1.0","operations":[
        {"id":"rename","type":"rename_program","partition_index":0,
         "volume_name":"Chain","program_number":33,"new_program_name":"Renamed"}
      ]})");
    ASSERT_TRUE(manifest) << manifest.error().message;
    const auto inspected = axk::inspect_hds_alteration(source, *manifest);
    ASSERT_TRUE(inspected) << inspected.error().message;
    const auto applied = axk::alter_hds(source, *manifest, output);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied->operations.front().allocated_clusters, 0U);
    EXPECT_EQ(applied->operations.front().freed_clusters, 0U);

    const auto after = axk::open_image(output);
    ASSERT_TRUE(after) << after.error().message;
    const auto after_catalog = axk::build_object_catalog(*after);
    ASSERT_TRUE(after_catalog) << after_catalog.error().message;
    const auto after_program = std::ranges::find_if(after_catalog->objects, [](const auto &object) {
        return object.object.header.type == axk::ObjectType::prog && object.object.header.name == "033";
    });
    ASSERT_NE(after_program, after_catalog->objects.end());
    const auto *decoded = std::get_if<axk::CurrentProg>(&after_program->object.payload);
    ASSERT_NE(decoded, nullptr);
    EXPECT_EQ(decoded->program_name, "Renamed");
    ASSERT_EQ(after_program->raw_payload.size(), before_payload.size());
    for (std::size_t offset = 0U; offset < before_payload.size(); ++offset) {
        if (offset < 0x78U || offset >= 0x80U) {
            EXPECT_EQ(after_program->raw_payload[offset], before_payload[offset])
                << "unexpected Program payload change at offset " << offset;
        }
    }

    const auto unchanged = axk::parse_alteration_manifest(R"({
      "schema_version":"1.0","operations":[
        {"id":"rename","type":"rename_program","partition_index":0,
         "volume_name":"Chain","program_number":33,"new_program_name":"Pgm 033"}
      ]})");
    ASSERT_TRUE(unchanged);
    EXPECT_FALSE(axk::inspect_hds_alteration(source, *unchanged));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, RenamePartitionChangesOnlySelectedMirroredHeaderName) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-rename-partition";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    axk::HdsBuildManifest manifest{"1.0", 8U * 1024U * 1024U, {}};
    manifest.partitions.push_back({"PARTITION 1", {}});
    manifest.partitions.push_back({"PARTITION 2", {}});
    ASSERT_TRUE(axk::write_hds_image(manifest, source));
    const auto before_image = axk::open_image(source);
    ASSERT_TRUE(before_image) << before_image.error().message;
    ASSERT_EQ(before_image->partitions().size(), 2U);
    const auto before_bytes = bytes(source);

    const auto alteration = axk::parse_alteration_manifest(R"({
      "schema_version":"1.0","operations":[
        {"id":"rename","type":"rename_partition","partition_index":1,
         "partition_name":"PARTITION 2    1","new_partition_name":"Samples"}
      ]})");
    ASSERT_TRUE(alteration) << alteration.error().message;
    const auto applied = axk::alter_hds(source, *alteration, output);
    ASSERT_TRUE(applied) << applied.error().message;

    const auto after_image = axk::open_image(output);
    ASSERT_TRUE(after_image) << after_image.error().message;
    ASSERT_EQ(after_image->partitions().size(), 2U);
    EXPECT_EQ(after_image->partitions()[0].name, "PARTITION 1");
    EXPECT_EQ(after_image->partitions()[1].name, "Samples");
    EXPECT_TRUE(after_image->partitions()[1].backup_header_matches);

    const auto after_bytes = bytes(output);
    ASSERT_EQ(after_bytes.size(), before_bytes.size());
    const auto partition_start = static_cast<std::size_t>(before_image->partitions()[1].start_sector) * 512U;
    for (std::size_t index = 0; index < before_bytes.size(); ++index) {
        const auto in_primary_name = index >= partition_start + 0x40U && index < partition_start + 0x50U;
        const auto in_backup_name = index >= partition_start + 1024U + 0x40U && index < partition_start + 1024U + 0x50U;
        if (!in_primary_name && !in_backup_name) {
            EXPECT_EQ(after_bytes[index], before_bytes[index]) << "unexpected byte change at " << index;
        }
    }

    const auto stale = axk::parse_alteration_manifest(R"({
      "schema_version":"1.0","operations":[
        {"id":"rename","type":"rename_partition","partition_index":1,
         "partition_name":"Wrong","new_partition_name":"Other"}
      ]})");
    ASSERT_TRUE(stale);
    EXPECT_FALSE(axk::alter_hds(source, *stale, root / "stale.hds"));
    EXPECT_FALSE(std::filesystem::exists(root / "stale.hds"));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, CancellationPublishesNothing) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-cancel";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_hds_image(source_manifest(), source));
    const auto manifest = axk::parse_alteration_manifest(
        R"({"schema_version":"1.0","operations":[{"id":"remove","type":"delete_volume","partition_index":0,"volume_name":"Removed"}]})");
    ASSERT_TRUE(manifest);
    axk::CancellationSource cancellation;
    cancellation.cancel();
    EXPECT_FALSE(axk::alter_hds(source, *manifest, output, cancellation.token()));
    EXPECT_FALSE(std::filesystem::exists(output));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, CancellationAfterEveryQueuedOperationPublishesNothing) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-cancel-queue";
    const auto source = root / "source.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_hds_image(source_manifest(), source));
    const auto source_before = bytes(source);
    const auto manifest = axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"first","type":"delete_volume","partition_index":0,"volume_name":"Removed"},
      {"id":"second","type":"delete_volume","partition_index":{"operation_ref":"first"},"volume_name":"Retained"}
    ]})");
    ASSERT_TRUE(manifest);
    for (std::uint64_t cancel_after = 1U; cancel_after <= 2U; ++cancel_after) {
        const auto output = root / std::format("cancel-{}.hds", cancel_after);
        axk::CancellationSource cancellation;
        CancellingProgress progress{cancellation, cancel_after};
        const auto result = axk::alter_hds(source, *manifest, output, cancellation.token(), &progress);
        EXPECT_FALSE(result);
        EXPECT_EQ(result.error().code, axk::ErrorCode::operation_cancelled);
        EXPECT_FALSE(std::filesystem::exists(output));
        EXPECT_EQ(bytes(source), source_before);
    }
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, ConcurrentPublishersUseUniqueTemporarySiblings) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-concurrent";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_hds_image(source_manifest(), source));
    const auto manifest = axk::parse_alteration_manifest(
        R"({"schema_version":"1.0","operations":[{"id":"remove","type":"delete_volume","partition_index":0,"volume_name":"Removed"}]})");
    ASSERT_TRUE(manifest);
    auto first = std::async(std::launch::async, [&] { return axk::alter_hds(source, *manifest, output).has_value(); });
    auto second = std::async(std::launch::async, [&] { return axk::alter_hds(source, *manifest, output).has_value(); });
    EXPECT_NE(first.get(), second.get());
    EXPECT_TRUE(std::filesystem::exists(output));
    EXPECT_FALSE(std::ranges::any_of(std::filesystem::directory_iterator{root}, [](const auto &entry) {
        return entry.path().filename().string().starts_with(".output.hds.alter.");
    }));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, RejectsSourceBitmapThatExposesLiveExtentsAsFree) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-stale-bitmap";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_hds_image(source_manifest(), source));
    const auto opened = axk::open_image(source);
    ASSERT_TRUE(opened);
    const auto &partition = opened->partitions()[0];
    const auto live = std::ranges::find_if(
        partition.records, [](const auto &record) { return record.sfs_id.value >= 3U && !record.extents.empty(); });
    ASSERT_NE(live, partition.records.end());
    mark_cluster_free(source, partition, live->extents[0].cluster_offset);
    const auto manifest = axk::parse_alteration_manifest(
        R"({"schema_version":"1.0","operations":[{"id":"delete","type":"delete_volume","partition_index":0,"volume_name":"Removed"}]})");
    ASSERT_TRUE(manifest);
    EXPECT_FALSE(axk::alter_hds(source, *manifest, output));
    EXPECT_FALSE(std::filesystem::exists(output));
    std::filesystem::remove_all(root, error);
}

#if !defined(_WIN32)
TEST(Alteration, RejectsOutputAliasesThroughASymlinkedParent) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-source-alias";
    const auto real = root / "real";
    const auto alias = root / "alias";
    const auto source = real / "source.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(real);
    ASSERT_TRUE(axk::write_hds_image(source_manifest(), source));
    std::filesystem::create_directory_symlink(real, alias);
    const auto original = bytes(source);
    const auto manifest = axk::parse_alteration_manifest(
        R"({"schema_version":"1.0","operations":[{"id":"delete","type":"delete_volume","partition_index":0,"volume_name":"Removed"}]})");
    ASSERT_TRUE(manifest);

    const auto altered = axk::alter_hds(source, *manifest, alias / "source.hds", {}, nullptr, true);
    ASSERT_FALSE(altered);
    EXPECT_EQ(altered.error().code, axk::ErrorCode::transaction_rejected);
    EXPECT_EQ(bytes(source), original);
    std::filesystem::remove_all(root, error);
}

TEST(PackageImport, RejectsOutputAliasesThroughASymlinkedParentBeforePlanEvaluation) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-package-import-source-alias";
    const auto real = root / "real";
    const auto alias = root / "alias";
    const auto source = real / "source.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(real);
    ASSERT_TRUE(axk::write_hds_image(source_manifest(), source));
    std::filesystem::create_directory_symlink(real, alias);
    const auto original = bytes(source);

    const axk::PackageImportPlan untrusted_plan;
    const auto applied = axk::apply_package_import(source, {}, untrusted_plan, alias / "source.hds", true);
    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().code, axk::ErrorCode::transaction_rejected);
    EXPECT_EQ(bytes(source), original);
    std::filesystem::remove_all(root, error);
}
#endif

TEST(Alteration, RejectsBitmapGeometryThatTargetsANeighboringPartitionWithoutPublishing) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-cross-partition-bitmap";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    auto build = source_manifest();
    build.partitions.push_back({"hd2", {}});
    ASSERT_TRUE(axk::write_hds_image(build, source));
    const auto opened = axk::open_image(source);
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened->partitions().size(), 2U);
    const auto &first = opened->partitions()[0];
    const auto &second = opened->partitions()[1];
    ASSERT_EQ(second.start_sector - first.start_sector, first.sector_count + 1U);
    const auto neighboring_cluster = (second.start_sector - first.start_sector) / first.sectors_per_cluster;
    patch_partition_header_be32(source, first, 0x9cU, neighboring_cluster);
    const auto original = bytes(source);

    const auto manifest = axk::parse_alteration_manifest(
        R"({"schema_version":"1.0","operations":[{"id":"delete","type":"delete_volume","partition_index":0,"volume_name":"Removed"}]})");
    ASSERT_TRUE(manifest);
    const auto altered = axk::alter_hds(source, *manifest, output);
    ASSERT_FALSE(altered);
    EXPECT_EQ(altered.error().code, axk::ErrorCode::transaction_rejected);
    EXPECT_EQ(altered.error().message, "source allocation geometry cannot safely support alteration");
    EXPECT_EQ(bytes(source), original);
    EXPECT_FALSE(std::filesystem::exists(output));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, RejectsSourceWithCrossLinkedRecordAllocation) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-cross-linked-source";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_hds_image(source_manifest(), source));
    const auto opened = axk::open_image(source);
    ASSERT_TRUE(opened);
    const auto &partition = opened->partitions().front();
    const axk::IndexRecord *first{};
    const axk::IndexRecord *second{};
    for (const auto &candidate : partition.records) {
        if (candidate.extents.size() != 1U)
            continue;
        const auto matching = std::ranges::find_if(partition.records, [&](const auto &other) {
            return other.sfs_id != candidate.sfs_id && other.extents.size() == 1U &&
                   other.extents.front().cluster_count == candidate.extents.front().cluster_count;
        });
        if (matching != partition.records.end()) {
            first = &candidate;
            second = &*matching;
            break;
        }
    }
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    patch_index_be32(source, *second, 0x0aU, first->extents.front().cluster_offset);
    const auto corrupted = axk::open_image(source);
    ASSERT_TRUE(corrupted);
    EXPECT_NE(corrupted->partitions().front().allocation.conflicting_cluster_count, 0U);

    const auto manifest = axk::parse_alteration_manifest(
        R"({"schema_version":"1.0","operations":[{"id":"delete","type":"delete_volume","partition_index":0,"volume_name":"Removed"}]})");
    ASSERT_TRUE(manifest);
    const auto altered = axk::alter_hds(source, *manifest, output);
    ASSERT_FALSE(altered);
    EXPECT_EQ(altered.error().code, axk::ErrorCode::transaction_rejected);
    EXPECT_EQ(altered.error().message, "source allocation cannot safely support alteration");
    EXPECT_FALSE(std::filesystem::exists(output));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, RenamesHardwareSizedSampleBankAndNormalizesProgramHandle) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-rename-wide-sample-bank";
    const auto audio = root / "tone.wav";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_wav_atomic(audio, test_waveform()));
    ASSERT_TRUE(axk::write_hds_image(wide_sample_bank_source_manifest(audio), source));

    const auto generated = axk::open_image(source);
    ASSERT_TRUE(generated) << generated.error().message;
    const auto &partition = generated->partitions().front();
    const auto group = std::ranges::find_if(partition.records, [](const auto &record) {
        return record.object_type == "SBAC" && record.object_name == "Group";
    });
    ASSERT_NE(group, partition.records.end());
    const auto program = std::ranges::find_if(partition.records, [](const auto &record) {
        return record.object_type == "PROG" && record.object_name == "033";
    });
    ASSERT_NE(program, partition.records.end());
    for (std::size_t index = 0U; index < 5U; ++index) {
        if (index >= 3U) {
            patch_record_name(source, partition, *group, 0x14cU + index * 0x14U, std::format("Member {}", index + 1U));
        }
        patch_record_be32(source, partition, *group, 0x15cU + index * 0x14U,
                          0x0144'0000U + static_cast<std::uint32_t>(index));
    }
    patch_record_byte(source, partition, *group, 0x144U, std::byte{5});
    for (std::string_view name : {"Member 4", "Member 5"}) {
        const auto member = std::ranges::find_if(partition.records, [&](const auto &record) {
            return record.object_type == "SBNK" && record.object_name == name;
        });
        ASSERT_NE(member, partition.records.end());
        patch_record_byte(source, partition, *member, 0xd0U, std::byte{3});
    }
    patch_record_be32(source, partition, *program, 0x130U, 0x0144'1000U);

    const auto before = axk::open_image(source);
    ASSERT_TRUE(before) << before.error().message;
    const auto before_catalog = axk::build_object_catalog(*before);
    ASSERT_TRUE(before_catalog) << before_catalog.error().message;
    const auto before_group = std::ranges::find_if(before_catalog->objects, [](const auto &object) {
        return object.object.header.type == axk::ObjectType::sbac && object.object.header.name == "Group";
    });
    ASSERT_NE(before_group, before_catalog->objects.end());
    const auto *before_decoded = std::get_if<axk::CurrentSbac>(&before_group->object.payload);
    ASSERT_NE(before_decoded, nullptr);
    ASSERT_EQ(before_decoded->slots.size(), 5U);

    const auto manifest = axk::parse_alteration_manifest(R"({
      "schema_version":"1.0","operations":[
        {"id":"rename","type":"rename_sbac","partition_index":0,
         "volume_name":"Wide Bank","sample_bank_name":"Group","new_sample_bank_name":"Renamed"}
      ]})");
    ASSERT_TRUE(manifest) << manifest.error().message;
    const auto inspected = axk::inspect_hds_alteration(source, *manifest);
    ASSERT_TRUE(inspected) << inspected.error().message;
    const auto applied = axk::alter_hds(source, *manifest, output);
    ASSERT_TRUE(applied) << applied.error().message;

    const auto after = axk::open_image(output);
    ASSERT_TRUE(after) << after.error().message;
    const auto after_catalog = axk::build_object_catalog(*after);
    ASSERT_TRUE(after_catalog) << after_catalog.error().message;
    const auto after_group = std::ranges::find_if(after_catalog->objects, [](const auto &object) {
        return object.object.header.type == axk::ObjectType::sbac && object.object.header.name == "Renamed";
    });
    ASSERT_NE(after_group, after_catalog->objects.end());
    const auto *after_decoded = std::get_if<axk::CurrentSbac>(&after_group->object.payload);
    ASSERT_NE(after_decoded, nullptr);
    ASSERT_EQ(after_decoded->slots.size(), 5U);
    for (std::size_t index = 0U; index < after_decoded->slots.size(); ++index) {
        EXPECT_EQ(after_decoded->slots[index].name, before_decoded->slots[index].name);
        EXPECT_EQ(after_decoded->slots[index].raw_handle, before_decoded->slots[index].raw_handle);
        EXPECT_EQ(after_decoded->slots[index].offset, before_decoded->slots[index].offset);
    }
    EXPECT_EQ(after_group->raw_payload.size(), before_group->raw_payload.size());
    for (std::size_t offset = 0U; offset < before_group->raw_payload.size(); ++offset) {
        if (offset < 0x32U || offset >= 0x42U) {
            EXPECT_EQ(after_group->raw_payload[offset], before_group->raw_payload[offset])
                << "unexpected SBAC payload change at offset " << offset;
        }
    }
    const auto after_program = std::ranges::find_if(after_catalog->objects, [](const auto &object) {
        return object.object.header.type == axk::ObjectType::prog && object.object.header.name == "033";
    });
    ASSERT_NE(after_program, after_catalog->objects.end());
    const auto *program_decoded = std::get_if<axk::CurrentProg>(&after_program->object.payload);
    ASSERT_NE(program_decoded, nullptr);
    ASSERT_FALSE(program_decoded->assignments.empty());
    EXPECT_EQ(program_decoded->assignments.front().name, "Renamed");
    EXPECT_EQ(program_decoded->assignments.front().raw_handle, 0U);
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, RejectsSharedSampleBankMember) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-sample-bank-safety";
    const auto audio = root / "tone.wav";
    const auto source = root / "source.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_wav_atomic(audio, test_waveform()));
    ASSERT_TRUE(axk::write_hds_image(chain_source_manifest(audio), source));
    const auto shared = axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"insert","type":"insert_sbac","partition_index":0,"volume_name":"Chain",
       "sample_bank":{"name":"Other Bank","member_samples":["Banked Sample"]}}
    ]})");
    ASSERT_TRUE(shared);
    EXPECT_FALSE(axk::inspect_hds_alteration(source, *shared));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, QueueReusesDeletedIdsAndAllocationForInsertedVolume) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-queue";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_hds_image(source_manifest(), source));
    const auto manifest = axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"delete","type":"delete_volume","partition_index":0,"volume_name":"Removed"},
      {"id":"insert","type":"insert_volume","partition_index":{"operation_ref":"delete"},
       "volume":{"name":"Replacement","waveforms":[],"samples":[]}}
    ]})");
    ASSERT_TRUE(manifest) << manifest.error().message;
    const auto applied = axk::alter_hds(source, *manifest, output);
    ASSERT_TRUE(applied) << applied.error().message;
    ASSERT_EQ(applied->operations.size(), 2U);
    EXPECT_EQ(applied->operations[0].removed_sfs_ids, applied->operations[1].inserted_sfs_ids);
    EXPECT_EQ(applied->operations[0].freed_clusters, applied->operations[1].allocated_clusters);
    const auto reopened = axk::open_image(output);
    ASSERT_TRUE(reopened);
    const auto &root_record =
        *std::ranges::find(reopened->partitions()[0].records, axk::SfsId{1}, &axk::IndexRecord::sfs_id);
    EXPECT_TRUE(std::ranges::any_of(root_record.directory_entries,
                                    [](const auto &entry) { return entry.name == "Replacement"; }));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, DeleteThenInsertSampleReusesRecordAndAllocation) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-sbnk";
    const auto audio = root / "tone.wav";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_wav_atomic(audio, test_waveform()));
    ASSERT_TRUE(axk::write_hds_image(sample_source_manifest(audio), source));
    const auto manifest = axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"delete","type":"delete_sbnk","partition_index":0,
       "volume_name":"Samples","sample_name":"Old Sample"},
      {"id":"insert","type":"insert_sbnk","partition_index":{"operation_ref":"delete"},
       "volume_name":"Samples","sample":{"name":"New Sample","waveform_name":"Wave",
       "root_key":64,"key_low":10,"key_high":100,"level":87}}
    ]})");
    ASSERT_TRUE(manifest) << manifest.error().message;
    const auto applied = axk::alter_hds(source, *manifest, output);
    ASSERT_TRUE(applied) << applied.error().message;
    ASSERT_EQ(applied->operations.size(), 2U);
    EXPECT_EQ(applied->operations[0].removed_sfs_ids, applied->operations[1].inserted_sfs_ids);
    EXPECT_EQ(applied->operations[0].freed_clusters, applied->operations[1].allocated_clusters);
    const auto reopened = axk::open_image(output);
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(catalog) << catalog.error().message;
    const auto sample = std::ranges::find_if(catalog->objects, [](const auto &object) {
        return object.placement && object.placement->entry_name == "New Sample";
    });
    ASSERT_NE(sample, catalog->objects.end());
    const auto *decoded = std::get_if<axk::CurrentSbnk>(&sample->object.payload);
    ASSERT_NE(decoded, nullptr);
    EXPECT_EQ(decoded->left.wave_data_name, "Wave");
    EXPECT_EQ(decoded->left.root_key, 64U);
    EXPECT_EQ(decoded->key_range_low, 10U);
    EXPECT_EQ(decoded->key_range_high, 100U);
    EXPECT_EQ(decoded->sample_level, 87U);
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, QueuedWaveformAndSampleInsertionUsesEvolvingState) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-wave-queue";
    const auto audio = root / "tone.wav";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    const auto transaction = root / "transaction.json";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    const auto expected_waveform = test_waveform();
    ASSERT_TRUE(axk::write_wav_atomic(audio, expected_waveform));
    axk::HdsBuildManifest source_spec{"1.0", 4U * 1024U * 1024U, {}};
    axk::VolumeSpec volume;
    volume.name = "Queue";
    source_spec.partitions.push_back({"hd1", {std::move(volume)}});
    ASSERT_TRUE(axk::write_hds_image(source_spec, source));
    const auto manifest = axk::parse_alteration_manifest(
        R"({"schema_version":"1.0","operations":[
        {"id":"wave","type":"insert_waveform","partition_index":0,
         "volume_name":"Queue","audio":{"path":"tone.wav","waveform_names":["Wave"],
         "root_key":60}},
        {"id":"sample","type":"insert_sbnk","partition_index":{"operation_ref":"wave"},
         "volume_name":"Queue","sample":{"name":"Sample","waveform_name":"Wave",
         "root_key":60,"key_low":0,"key_high":127}}
      ]})",
        root);
    ASSERT_TRUE(manifest) << manifest.error().message;
    const auto applied = axk::alter_hds(source, *manifest, output);
    ASSERT_TRUE(applied) << applied.error().message;
    ASSERT_EQ(applied->operations.size(), 2U);
    const auto reopened = axk::open_image(output);
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(catalog) << catalog.error().message;
    const auto waveform = std::ranges::find_if(catalog->objects, [](const auto &object) {
        return object.object.header.type == axk::ObjectType::smpl && object.object.header.name == "Wave";
    });
    ASSERT_NE(waveform, catalog->objects.end());
    const auto decoded_waveform = axk::decode_waveform(*reopened, *waveform);
    ASSERT_TRUE(decoded_waveform) << decoded_waveform.error().message;
    auto expected_stored_pcm = expected_waveform.pcm;
    expected_stored_pcm.insert(expected_stored_pcm.end(), expected_waveform.pcm.begin(), expected_waveform.pcm.end());
    EXPECT_EQ(decoded_waveform->pcm, expected_stored_pcm);
    EXPECT_TRUE(std::ranges::any_of(catalog->objects, [](const auto &object) {
        return object.object.header.type == axk::ObjectType::sbnk && object.object.header.name == "Sample";
    }));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, GrowsCategoryDirectoryWhenQueuedWaveDataExceedsItsInitialCapacity) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-wave-directory-growth";
    const auto audio = root / "tone.wav";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_wav_atomic(audio, test_waveform()));
    axk::HdsBuildManifest source_spec{"1.0", 4U * 1024U * 1024U, {}};
    axk::VolumeSpec volume;
    volume.name = "Queue";
    source_spec.partitions.push_back({"hd1", {std::move(volume)}});
    ASSERT_TRUE(axk::write_hds_image(source_spec, source));

    axk::AlterationManifest manifest{"1.0", {}};
    constexpr std::size_t waveform_count = 63U;
    manifest.operations.reserve(waveform_count);
    for (std::size_t index = 0U; index < waveform_count; ++index) {
        const auto name = std::format("Wave {:02}", index);
        axk::InsertWaveformSpec waveform;
        waveform.path = audio;
        waveform.waveform_names = {name};
        waveform.root_key = 60U;
        manifest.operations.push_back(
            {std::format("wave-{:02}", index),
             axk::InsertWaveformOperation{axk::PartitionIndex{0U}, "Queue", std::move(waveform)}});
    }

    const auto applied = axk::alter_hds(source, manifest, output);
    ASSERT_TRUE(applied) << applied.error().message;
    ASSERT_EQ(applied->operations.size(), waveform_count);
    const auto reopened = axk::open_image(output);
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto &records = reopened->partitions().front().records;
    const auto directory = std::ranges::find_if(records, [](const auto &record) {
        return record.payload_kind == axk::PayloadKind::directory &&
               std::ranges::any_of(record.directory_entries, [](const auto &entry) { return entry.name == "Wave 62"; });
    });
    ASSERT_NE(directory, records.end());
    EXPECT_EQ(directory->directory_entries.size(), waveform_count + 2U);
    EXPECT_EQ(directory->data_size, (waveform_count + 2U) * 32U);
    EXPECT_GE(directory->cluster_count, 3U);
    const auto catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(catalog) << catalog.error().message;
    EXPECT_TRUE(catalog->issues.empty());
    EXPECT_EQ(std::ranges::count_if(catalog->objects,
                                    [](const auto &object) {
                                        return object.object.header.type == axk::ObjectType::smpl &&
                                               object.placement_resolution == axk::PlacementResolution::exact &&
                                               object.placement && object.placement->volume_name == "Queue" &&
                                               object.placement->category_name == "SMPL";
                                    }),
              waveform_count);
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, GrowsCategoryDirectoryWhenQueuedSamplesExceedItsInitialCapacity) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-sample-directory-growth";
    const auto audio = root / "tone.wav";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_wav_atomic(audio, test_waveform()));
    auto source_spec = sample_source_manifest(audio);
    source_spec.partitions.front().volumes.front().samples.clear();
    ASSERT_TRUE(axk::write_hds_image(source_spec, source));

    axk::AlterationManifest manifest{"1.0", {}};
    constexpr std::size_t sample_count = 63U;
    manifest.operations.reserve(sample_count);
    for (std::size_t index = 0U; index < sample_count; ++index) {
        axk::SampleSpec sample;
        sample.name = std::format("Sample {:02}", index);
        sample.waveform_id = "Wave";
        sample.root_key = 60U;
        sample.key_high = 127U;
        manifest.operations.push_back(
            {std::format("sample-{:02}", index),
             axk::InsertSampleOperation{axk::PartitionIndex{0U}, "Samples", std::move(sample)}});
    }

    const auto applied = axk::alter_hds(source, manifest, output);
    ASSERT_TRUE(applied) << applied.error().message;
    ASSERT_EQ(applied->operations.size(), sample_count);
    const auto reopened = axk::open_image(output);
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto &records = reopened->partitions().front().records;
    const auto directory = std::ranges::find_if(records, [](const auto &record) {
        return record.payload_kind == axk::PayloadKind::directory &&
               std::ranges::any_of(record.directory_entries,
                                   [](const auto &entry) { return entry.name == "Sample 62"; });
    });
    ASSERT_NE(directory, records.end());
    EXPECT_EQ(directory->directory_entries.size(), sample_count + 2U);
    EXPECT_EQ(directory->data_size, (sample_count + 2U) * 32U);
    EXPECT_GE(directory->cluster_count, 3U);
    const auto catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(catalog) << catalog.error().message;
    EXPECT_TRUE(catalog->issues.empty());
    EXPECT_EQ(std::ranges::count_if(catalog->objects,
                                    [](const auto &object) {
                                        return object.object.header.type == axk::ObjectType::sbnk &&
                                               object.placement_resolution == axk::PlacementResolution::exact &&
                                               object.placement && object.placement->volume_name == "Samples" &&
                                               object.placement->category_name == "SBNK";
                                    }),
              sample_count);
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, PostWritePlacementValidationToleratesUnchangedBaselineIssues) {
    axk::ObjectCatalog before;
    before.issues.push_back({"CATALOG_OBJECT_PLACEMENT_MISSING", "existing", axk::PartitionIndex{0U}, axk::SfsId{7U}});
    auto after = before;

    const auto validated = axk::alteration_internal::validate_post_write_placements(before, after, {});
    EXPECT_TRUE(validated) << validated.error().message;
}

TEST(Alteration, PostWritePlacementValidationRejectsNewAndUnplacedInsertedObjects) {
    axk::ObjectCatalog before;
    axk::ObjectCatalog after;
    after.issues.push_back({"CATALOG_OBJECT_PLACEMENT_MISSING", "new", axk::PartitionIndex{0U}, axk::SfsId{8U}});

    const auto newly_unplaced = axk::alteration_internal::validate_post_write_placements(before, after, {});
    ASSERT_FALSE(newly_unplaced);
    EXPECT_EQ(newly_unplaced.error().message,
              "post-write introduced CATALOG_OBJECT_PLACEMENT_MISSING for partition 0 SFS ID 8");

    const std::array expected{
        axk::alteration_internal::ExpectedObjectPlacement{axk::PartitionIndex{0U}, axk::SfsId{9U}, "Target"}};
    const auto missing_inserted = axk::alteration_internal::validate_post_write_placements(before, before, expected);
    ASSERT_FALSE(missing_inserted);
    EXPECT_EQ(missing_inserted.error().message, "post-write object in partition 0 SFS ID 9 could not be reopened");
}

TEST(Alteration, RepairsExplicitlySelectedMissingObjectPlacementWithoutChangingPayload) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-placement-repair";
    const auto audio = root / "tone.wav";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_wav_atomic(audio, test_waveform()));
    ASSERT_TRUE(axk::write_hds_image(sample_source_manifest(audio), source));

    auto opened = axk::open_image(source);
    ASSERT_TRUE(opened) << opened.error().message;
    const auto &partition = opened->partitions().front();
    const auto sample_directory = std::ranges::find_if(partition.records, [](const auto &record) {
        return record.payload_kind == axk::PayloadKind::directory &&
               std::ranges::any_of(record.directory_entries,
                                   [](const auto &entry) { return entry.name == "Old Sample"; });
    });
    ASSERT_NE(sample_directory, partition.records.end());
    ASSERT_EQ(sample_directory->directory_entries.size(), 3U);
    const auto sample_entry =
        std::ranges::find(sample_directory->directory_entries, "Old Sample", &axk::DirectoryEntry::name);
    ASSERT_NE(sample_entry, sample_directory->directory_entries.end());
    const auto sample_sfs_id = axk::SfsId{sample_entry->link_id.value};
    patch_index_be32(source, *sample_directory, 6U, 64U);

    opened = axk::open_image(source);
    ASSERT_TRUE(opened) << opened.error().message;
    const auto before = axk::build_object_catalog(*opened);
    ASSERT_TRUE(before) << before.error().message;
    const auto unplaced = std::ranges::find_if(before->objects, [&](const auto &object) {
        return object.partition.value == 0U && object.sfs_id == sample_sfs_id;
    });
    ASSERT_NE(unplaced, before->objects.end());
    ASSERT_EQ(unplaced->placement_resolution, axk::PlacementResolution::missing);
    const auto expected_payload = unplaced->raw_payload;

    axk::AlterationManifest manifest{
        "1.0",
        {{"repair", axk::RepairObjectPlacementsOperation{axk::PartitionIndex{0U}, "Samples", {sample_sfs_id}}}},
    };
    const auto applied = axk::alter_hds(source, manifest, output);
    ASSERT_TRUE(applied) << applied.error().message;
    ASSERT_EQ(applied->operations.size(), 1U);
    EXPECT_EQ(applied->operations.front().placed_sfs_ids, std::vector{sample_sfs_id});

    const auto repaired = axk::open_image(output);
    ASSERT_TRUE(repaired) << repaired.error().message;
    const auto after = axk::build_object_catalog(*repaired);
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_TRUE(after->issues.empty());
    const auto placed = std::ranges::find_if(after->objects, [&](const auto &object) {
        return object.partition.value == 0U && object.sfs_id == sample_sfs_id;
    });
    ASSERT_NE(placed, after->objects.end());
    ASSERT_TRUE(placed->placement);
    EXPECT_EQ(placed->placement_resolution, axk::PlacementResolution::exact);
    EXPECT_EQ(placed->placement->volume_name, "Samples");
    EXPECT_EQ(placed->placement->category_name, "SBNK");
    EXPECT_EQ(placed->raw_payload, expected_payload);

    const auto graph = axk::build_relationship_graph(*after);
    EXPECT_TRUE(std::ranges::any_of(graph.relationships, [&](const auto &relationship) {
        return relationship.source_key == placed->key && relationship.quality == axk::RelationshipQuality::known;
    }));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, AtomicallyCreatesRecoveryVolumeAndRepairsOwnerlessObjectPlacement) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-placement-recovery-volume";
    const auto audio = root / "tone.wav";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_wav_atomic(audio, test_waveform()));
    auto source_spec = sample_source_manifest(audio);
    source_spec.partitions.front().volumes.front().samples.clear();
    ASSERT_TRUE(axk::write_hds_image(source_spec, source));

    auto opened = axk::open_image(source);
    ASSERT_TRUE(opened) << opened.error().message;
    const auto &partition = opened->partitions().front();
    const auto waveform_directory = std::ranges::find_if(partition.records, [](const auto &record) {
        return record.payload_kind == axk::PayloadKind::directory &&
               std::ranges::any_of(record.directory_entries, [](const auto &entry) { return entry.name == "Wave"; });
    });
    ASSERT_NE(waveform_directory, partition.records.end());
    ASSERT_EQ(waveform_directory->directory_entries.size(), 3U);
    const auto waveform_entry =
        std::ranges::find(waveform_directory->directory_entries, "Wave", &axk::DirectoryEntry::name);
    ASSERT_NE(waveform_entry, waveform_directory->directory_entries.end());
    const auto waveform_sfs_id = axk::SfsId{waveform_entry->link_id.value};
    patch_index_be32(source, *waveform_directory, 6U, 64U);

    opened = axk::open_image(source);
    ASSERT_TRUE(opened) << opened.error().message;
    const auto before = axk::build_object_catalog(*opened);
    ASSERT_TRUE(before) << before.error().message;
    const auto unplaced = std::ranges::find_if(before->objects, [&](const auto &object) {
        return object.partition.value == 0U && object.sfs_id == waveform_sfs_id;
    });
    ASSERT_NE(unplaced, before->objects.end());
    ASSERT_EQ(unplaced->placement_resolution, axk::PlacementResolution::missing);
    const auto expected_payload = unplaced->raw_payload;

    axk::VolumeSpec recovered;
    recovered.name = "Recovered";
    axk::AlterationManifest manifest{
        "1.0",
        {{"create-recovery-volume", axk::InsertVolumeOperation{axk::PartitionIndex{0U}, std::move(recovered)}},
         {"repair-ownerless-wave-data",
          axk::RepairObjectPlacementsOperation{axk::PartitionIndex{0U}, "Recovered", {waveform_sfs_id}}}},
    };
    const auto applied = axk::alter_hds(source, manifest, output);
    ASSERT_TRUE(applied) << applied.error().message;
    ASSERT_EQ(applied->operations.size(), 2U);
    EXPECT_EQ(applied->operations.back().placed_sfs_ids, std::vector{waveform_sfs_id});

    const auto repaired = axk::open_image(output);
    ASSERT_TRUE(repaired) << repaired.error().message;
    const auto after = axk::build_object_catalog(*repaired);
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_TRUE(after->issues.empty());
    const auto placed = std::ranges::find_if(after->objects, [&](const auto &object) {
        return object.partition.value == 0U && object.sfs_id == waveform_sfs_id;
    });
    ASSERT_NE(placed, after->objects.end());
    ASSERT_TRUE(placed->placement);
    EXPECT_EQ(placed->placement_resolution, axk::PlacementResolution::exact);
    EXPECT_EQ(placed->placement->volume_name, "Recovered");
    EXPECT_EQ(placed->placement->category_name, "SMPL");
    EXPECT_EQ(placed->raw_payload, expected_payload);
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, RecoversDirectoryEntriesHiddenByAStaleExtentByteCount) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-placement-byte-count-repair";
    const auto audio = root / "tone.wav";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_wav_atomic(audio, test_waveform()));
    ASSERT_TRUE(axk::write_hds_image(sample_source_manifest(audio), source));

    auto opened = axk::open_image(source);
    ASSERT_TRUE(opened) << opened.error().message;
    const auto &partition = opened->partitions().front();
    const auto sample_directory = std::ranges::find_if(partition.records, [](const auto &record) {
        return record.payload_kind == axk::PayloadKind::directory &&
               std::ranges::any_of(record.directory_entries,
                                   [](const auto &entry) { return entry.name == "Old Sample"; });
    });
    ASSERT_NE(sample_directory, partition.records.end());
    const auto sample_entry =
        std::ranges::find(sample_directory->directory_entries, "Old Sample", &axk::DirectoryEntry::name);
    ASSERT_NE(sample_entry, sample_directory->directory_entries.end());
    const auto sample_sfs_id = axk::SfsId{sample_entry->link_id.value};
    const auto sample_directory_sfs_id = sample_directory->sfs_id;
    patch_index_be32(source, *sample_directory, 0x12U, 64U);

    opened = axk::open_image(source);
    ASSERT_TRUE(opened) << opened.error().message;
    const auto before = axk::build_object_catalog(*opened);
    ASSERT_TRUE(before) << before.error().message;
    const auto unplaced = std::ranges::find_if(before->objects, [&](const auto &object) {
        return object.partition.value == 0U && object.sfs_id == sample_sfs_id;
    });
    ASSERT_NE(unplaced, before->objects.end());
    ASSERT_EQ(unplaced->placement_resolution, axk::PlacementResolution::missing);
    const auto expected_payload = unplaced->raw_payload;

    const axk::AlterationManifest manifest{
        "1.0",
        {{"repair", axk::RepairObjectPlacementsOperation{axk::PartitionIndex{0U}, "Samples", {sample_sfs_id}}}},
    };
    const auto applied = axk::alter_hds(source, manifest, output);
    ASSERT_TRUE(applied) << applied.error().message;

    const auto repaired = axk::open_image(output);
    ASSERT_TRUE(repaired) << repaired.error().message;
    const auto after = axk::build_object_catalog(*repaired);
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_TRUE(after->issues.empty());
    const auto placed = std::ranges::find_if(after->objects, [&](const auto &object) {
        return object.partition.value == 0U && object.sfs_id == sample_sfs_id;
    });
    ASSERT_NE(placed, after->objects.end());
    EXPECT_EQ(placed->placement_resolution, axk::PlacementResolution::exact);
    EXPECT_EQ(placed->raw_payload, expected_payload);
    const auto repaired_directory =
        std::ranges::find(repaired->partitions().front().records, sample_directory_sfs_id, &axk::IndexRecord::sfs_id);
    ASSERT_NE(repaired_directory, repaired->partitions().front().records.end());
    ASSERT_EQ(repaired_directory->extents.size(), 1U);
    EXPECT_EQ(repaired_directory->extents.front().byte_count, repaired_directory->data_size);
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, WaveformDeletionRequiresPriorSampleDeletion) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-wave-delete";
    const auto audio = root / "tone.wav";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_wav_atomic(audio, test_waveform()));
    ASSERT_TRUE(axk::write_hds_image(sample_source_manifest(audio), source));
    const auto rejected = axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"wave","type":"delete_waveform","partition_index":0,
       "volume_name":"Samples","waveform_name":"Wave"}
    ]})");
    ASSERT_TRUE(rejected);
    EXPECT_FALSE(axk::inspect_hds_alteration(source, *rejected));
    const auto accepted = axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"sample","type":"delete_sbnk","partition_index":0,
       "volume_name":"Samples","sample_name":"Old Sample"},
      {"id":"wave","type":"delete_waveform","partition_index":{"operation_ref":"sample"},
       "volume_name":"Samples","waveform_name":"Wave"}
    ]})");
    ASSERT_TRUE(accepted);
    ASSERT_TRUE(axk::alter_hds(source, *accepted, output));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, WaveformRenameUsesMemberNameAndRefreshesStaleCachedReference) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-wave-rename-stale-cache";
    const auto audio = root / "tone.wav";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_wav_atomic(audio, test_waveform()));
    ASSERT_TRUE(axk::write_hds_image(sample_source_manifest(audio), source));

    const auto opened = axk::open_image(source);
    ASSERT_TRUE(opened) << opened.error().message;
    const auto &partition = opened->partitions().front();
    const auto sample_record = std::ranges::find_if(partition.records, [](const auto &record) {
        return record.object_type == "SBNK" && record.object_name == "Old Sample";
    });
    const auto wave_data_record = std::ranges::find_if(partition.records, [](const auto &record) {
        return record.object_type == "SMPL" && record.object_name == "Wave";
    });
    ASSERT_NE(sample_record, partition.records.end());
    ASSERT_NE(wave_data_record, partition.records.end());
    const auto catalog = axk::build_object_catalog(*opened);
    ASSERT_TRUE(catalog) << catalog.error().message;
    const auto wave_data = std::ranges::find_if(catalog->objects, [](const auto &object) {
        return object.object.header.type == axk::ObjectType::smpl && object.object.header.name == "Wave";
    });
    ASSERT_NE(wave_data, catalog->objects.end());
    const auto *decoded_wave_data = std::get_if<axk::CurrentSmpl>(&wave_data->object.payload);
    ASSERT_NE(decoded_wave_data, nullptr);
    const auto stale_cache = decoded_wave_data->wave_data_reference_value.value + 0x100U;
    patch_record_be32(source, partition, *sample_record, 0xa0U, stale_cache);

    const auto stale_opened = axk::open_image(source);
    ASSERT_TRUE(stale_opened) << stale_opened.error().message;
    const auto stale_catalog = axk::build_object_catalog(*stale_opened);
    ASSERT_TRUE(stale_catalog) << stale_catalog.error().message;
    const auto stale_graph = axk::build_relationship_graph(*stale_catalog);
    const auto known = std::ranges::find_if(stale_graph.relationships, [](const auto &relationship) {
        return relationship.type == "SBNK_LEFT_MEMBER_TO_SMPL";
    });
    ASSERT_NE(known, stale_graph.relationships.end());
    ASSERT_TRUE(known->target_key);
    EXPECT_EQ(known->quality, axk::RelationshipQuality::known);
    EXPECT_EQ(known->basis, "sbnk-member-name+same-volume");

    const auto manifest = axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"rename","type":"rename_waveform","partition_index":0,
       "volume_name":"Samples","waveform_name":"Wave","new_waveform_name":"Renamed Wave"}
    ]})");
    ASSERT_TRUE(manifest) << manifest.error().message;
    const auto renamed = axk::alter_hds(source, *manifest, output);
    ASSERT_TRUE(renamed) << renamed.error().message;

    const auto reopened = axk::open_image(output);
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto renamed_catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(renamed_catalog) << renamed_catalog.error().message;
    const auto renamed_wave_data = std::ranges::find_if(renamed_catalog->objects, [](const auto &object) {
        return object.object.header.type == axk::ObjectType::smpl && object.object.header.name == "Renamed Wave";
    });
    const auto renamed_sample = std::ranges::find_if(renamed_catalog->objects, [](const auto &object) {
        return object.object.header.type == axk::ObjectType::sbnk && object.object.header.name == "Old Sample";
    });
    ASSERT_NE(renamed_wave_data, renamed_catalog->objects.end());
    ASSERT_NE(renamed_sample, renamed_catalog->objects.end());
    const auto *renamed_wave = std::get_if<axk::CurrentSmpl>(&renamed_wave_data->object.payload);
    const auto *sample = std::get_if<axk::CurrentSbnk>(&renamed_sample->object.payload);
    ASSERT_NE(renamed_wave, nullptr);
    ASSERT_NE(sample, nullptr);
    EXPECT_EQ(sample->left.wave_data_name, "Renamed Wave");
    EXPECT_EQ(sample->left.cached_wave_data_reference_value, renamed_wave->wave_data_reference_value.value);
    EXPECT_NE(sample->left.cached_wave_data_reference_value, stale_cache);
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, StaleCachedReferenceDoesNotCreateCrossVolumeWaveDataDependency) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-cross-volume-waveform";
    const auto audio = root / "tone.wav";
    const auto source = root / "source.hds";
    const auto shared = root / "shared.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_wav_atomic(audio, test_waveform()));

    axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {}};
    axk::VolumeSpec volume_a;
    volume_a.name = "Volume A";
    volume_a.waveforms.push_back({"shared-a", "Shared Wave", audio, 60U, {}});
    axk::SampleSpec sample_a_spec;
    sample_a_spec.name = "Sample A";
    sample_a_spec.waveform_id = "shared-a";
    sample_a_spec.root_key = 60U;
    sample_a_spec.key_high = 127U;
    volume_a.samples.push_back(std::move(sample_a_spec));
    axk::VolumeSpec volume_b;
    volume_b.name = "Volume B";
    volume_b.waveforms.push_back({"unused-b", "Unused Wave", audio, 60U, {}});
    axk::SampleSpec sample_b_spec;
    sample_b_spec.name = "Sample B";
    sample_b_spec.waveform_id = "unused-b";
    sample_b_spec.root_key = 67U;
    sample_b_spec.key_high = 127U;
    volume_b.samples.push_back(std::move(sample_b_spec));
    manifest.partitions.push_back({"hd1", {std::move(volume_a), std::move(volume_b)}});
    ASSERT_TRUE(axk::write_hds_image(manifest, source));

    const auto container = axk::open_image(source);
    ASSERT_TRUE(container) << container.error().message;
    const auto catalog = axk::build_object_catalog(*container);
    ASSERT_TRUE(catalog) << catalog.error().message;
    const auto wave_data_a = std::ranges::find_if(catalog->objects, [](const auto &object) {
        return object.placement && object.placement->volume_name == "Volume A" &&
               object.object.header.type == axk::ObjectType::smpl && object.object.header.name == "Shared Wave";
    });
    const auto sample_b_object = std::ranges::find_if(catalog->objects, [](const auto &object) {
        return object.placement && object.placement->volume_name == "Volume B" &&
               object.object.header.type == axk::ObjectType::sbnk && object.object.header.name == "Sample B";
    });
    const auto wave_data_b = std::ranges::find_if(catalog->objects, [](const auto &object) {
        return object.placement && object.placement->volume_name == "Volume B" &&
               object.object.header.type == axk::ObjectType::smpl && object.object.header.name == "Unused Wave";
    });
    ASSERT_NE(wave_data_a, catalog->objects.end());
    ASSERT_NE(sample_b_object, catalog->objects.end());
    ASSERT_NE(wave_data_b, catalog->objects.end());
    const auto *decoded_wave_data = std::get_if<axk::CurrentSmpl>(&wave_data_a->object.payload);
    ASSERT_NE(decoded_wave_data, nullptr);
    const auto &partition = container->partitions().front();
    const auto record_b = std::ranges::find(partition.records, sample_b_object->sfs_id, &axk::IndexRecord::sfs_id);
    const auto wave_data_record_b =
        std::ranges::find(partition.records, wave_data_b->sfs_id, &axk::IndexRecord::sfs_id);
    ASSERT_NE(record_b, partition.records.end());
    ASSERT_NE(wave_data_record_b, partition.records.end());
    const auto disposable_link = decoded_wave_data->wave_data_reference_value.value + 1U;
    patch_record_be32(source, partition, *wave_data_record_b, 0x6cU, disposable_link - 0xbaU);
    patch_record_be32(source, partition, *wave_data_record_b, 0x78U, disposable_link);
    patch_record_name(source, partition, *record_b, 0x78U, "Shared Wave");
    patch_record_be32(source, partition, *record_b, 0xa0U, decoded_wave_data->wave_data_reference_value.value);

    const auto remove_duplicate = axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"wave","type":"delete_waveform","partition_index":0,
       "volume_name":"Volume B","waveform_name":"Unused Wave"}
    ]})");
    ASSERT_TRUE(remove_duplicate);
    const auto removed_duplicate = axk::alter_hds(source, *remove_duplicate, shared);
    ASSERT_TRUE(removed_duplicate) << removed_duplicate.error().message;

    const auto reopened = axk::open_image(shared);
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto shared_catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(shared_catalog) << shared_catalog.error().message;
    EXPECT_EQ(std::ranges::count_if(shared_catalog->objects,
                                    [](const auto &object) {
                                        return object.object.header.type == axk::ObjectType::smpl &&
                                               object.object.header.name == "Shared Wave";
                                    }),
              1);
    const auto graph = axk::build_relationship_graph(*shared_catalog);
    const auto stale_cache = std::ranges::find_if(graph.relationships, [&](const auto &relation) {
        if (relation.type != "SBNK_LEFT_MEMBER_TO_SMPL")
            return false;
        const auto source_object =
            std::ranges::find(shared_catalog->objects, relation.source_key, &axk::ObjectSnapshot::key);
        return source_object != shared_catalog->objects.end() && source_object->placement &&
               source_object->placement->volume_name == "Volume B";
    });
    ASSERT_NE(stale_cache, graph.relationships.end());
    EXPECT_FALSE(stale_cache->target_key);
    EXPECT_EQ(stale_cache->quality, axk::RelationshipQuality::tentative);
    EXPECT_EQ(stale_cache->basis, "sbnk-member-name-nonlocal-or-ambiguous");

    const auto delete_owner = axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"volume","type":"delete_volume","partition_index":0,
       "volume_name":"Volume A"}
    ]})");
    ASSERT_TRUE(delete_owner);
    const auto inspected = axk::inspect_hds_alteration(shared, *delete_owner);
    ASSERT_TRUE(inspected) << inspected.error().message;
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, WritesAndReopensFortyEightExtentContinuationList) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-fragmented";
    const auto audio = root / "large.wav";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    axk::Waveform waveform;
    waveform.format = {1U, 2U, 44100U};
    waveform.frame_count = 24'000U;
    waveform.pcm.resize(static_cast<std::size_t>(waveform.frame_count) * 2U);
    ASSERT_TRUE(axk::write_wav_atomic(audio, waveform));
    axk::HdsBuildManifest source_spec{"1.0", 4U * 1024U * 1024U, {}};
    axk::VolumeSpec volume;
    volume.name = "Fragmented";
    source_spec.partitions.push_back({"hd1", {std::move(volume)}});
    ASSERT_TRUE(axk::write_hds_image(source_spec, source));
    const auto opened = axk::open_image(source);
    ASSERT_TRUE(opened);
    const auto &partition = opened->partitions()[0];
    auto first_free = partition.directory_index_cluster + partition.directory_index_span_clusters;
    while (std::ranges::any_of(partition.records, [&](const auto &record) {
        return std::ranges::any_of(record.extents, [&](const auto &extent) {
            return first_free >= extent.cluster_offset && first_free < extent.cluster_offset + extent.cluster_count;
        });
    })) {
        ++first_free;
    }
    std::set<std::uint32_t> used_ids;
    for (const auto &record : partition.records)
        used_ids.insert(record.sfs_id.value);
    std::uint32_t next_id = 3U;
    for (std::uint32_t index = 0; index < 48U; ++index) {
        while (used_ids.contains(next_id))
            ++next_id;
        add_single_cluster_record(source, partition, axk::SfsId{next_id}, first_free + index * 2U + 1U);
        used_ids.insert(next_id++);
    }
    const auto manifest = axk::parse_alteration_manifest(
        R"({"schema_version":"1.0","operations":[
        {"id":"wave","type":"insert_waveform","partition_index":0,
         "volume_name":"Fragmented","audio":{"path":"large.wav",
         "waveform_names":["Large Wave"],"root_key":60}}
      ]})",
        root);
    ASSERT_TRUE(manifest) << manifest.error().message;
    const auto altered = axk::alter_hds(source, *manifest, output);
    ASSERT_TRUE(altered) << altered.error().message;
    EXPECT_EQ(altered->operations[0].allocated_clusters, 49U);
    const auto reopened = axk::open_image(output);
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto &records = reopened->partitions()[0].records;
    const auto inserted =
        std::ranges::find_if(records, [](const auto &record) { return record.object_name == "Large Wave"; });
    ASSERT_NE(inserted, records.end());
    EXPECT_EQ(inserted->extents.size(), 48U);
    EXPECT_EQ(inserted->continuation_clusters.size(), 1U);
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, DirectoryGrowthPreservesLogicalExtentOrderAcrossEarlierFreeSpace) {
    const std::array existing{
        axk::Extent{100U, 2U, 2048U},
        axk::Extent{120U, 1U, 1024U},
    };
    const std::array added{
        axk::Extent{10U, 1U, 1024U},
        axk::Extent{11U, 1U, 1024U},
    };

    const auto merged = axk::alteration_internal::merge_extents(existing, added);

    ASSERT_EQ(merged.size(), 3U);
    EXPECT_EQ(merged[0].cluster_offset, 100U);
    EXPECT_EQ(merged[0].cluster_count, 2U);
    EXPECT_EQ(merged[1].cluster_offset, 120U);
    EXPECT_EQ(merged[1].cluster_count, 1U);
    EXPECT_EQ(merged[2].cluster_offset, 10U);
    EXPECT_EQ(merged[2].cluster_count, 2U);
    EXPECT_EQ(merged[2].byte_count, 2048U);
}

TEST(Alteration, ContinuationExtentByteCountsCoverTheLogicalDirectoryPayload) {
    std::array extents{
        axk::Extent{4684U, 2U, 64U},   axk::Extent{5074U, 1U, 1024U}, axk::Extent{5270U, 1U, 1024U},
        axk::Extent{5459U, 1U, 1024U}, axk::Extent{5654U, 1U, 1024U}, axk::Extent{5850U, 1U, 1024U},
    };

    const auto normalized = axk::alteration_internal::normalize_extent_byte_counts(extents, 6464U);

    ASSERT_TRUE(normalized) << normalized.error().message;
    EXPECT_EQ(extents[0].byte_count, 2048U);
    EXPECT_EQ(extents[5].byte_count, 320U);
    EXPECT_EQ(
        std::ranges::fold_left(
            extents, 0U, [](std::uint32_t total, const axk::Extent &extent) { return total + extent.byte_count; }),
        6464U);
}
