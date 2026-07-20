#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iterator>
#include <ranges>

#include <gtest/gtest.h>

#include "axklib/alteration.hpp"
#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/relationship.hpp"
#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

namespace {

std::vector<char> bytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
}

axk::HdsBuildManifest source_manifest() {
    axk::HdsBuildManifest result{"1.1", 4U * 1024U * 1024U, {}};
    axk::VolumeSpec retained;
    retained.name = "Retained";
    axk::VolumeSpec removed;
    removed.name = "Removed";
    result.partitions.push_back({"hd1", {std::move(retained), std::move(removed)}});
    return result;
}

axk::HdsBuildManifest sample_source_manifest(const std::filesystem::path &audio_path) {
    axk::HdsBuildManifest result{"1.1", 4U * 1024U * 1024U, {}};
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

axk::Waveform test_waveform() {
    axk::Waveform result;
    result.format = {1U, 2U, 44100U};
    result.frame_count = 4U;
    result.pcm = {std::byte{0},    std::byte{0},    std::byte{0xe8}, std::byte{3},
                  std::byte{0x18}, std::byte{0xfc}, std::byte{0},    std::byte{0}};
    return result;
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

void patch_record_name(const std::filesystem::path &path, const axk::Partition &partition,
                       const axk::IndexRecord &record, std::size_t payload_offset, std::string_view name) {
    ASSERT_LE(name.size(), 16U);
    for (std::size_t index = 0; index < 16U; ++index) {
        patch_record_byte(path, partition, record, payload_offset + index,
                          index < name.size() ? static_cast<std::byte>(name[index]) : std::byte{' '});
    }
}

} // namespace

TEST(AlterationManifest, RequiresStrictOrderedBackwardReferences) {
    constexpr std::string_view valid = R"({
    "schema_version":"1.1","operations":[
      {"id":"first","type":"delete_volume","partition_index":0,"volume_name":"Removed"},
      {"id":"second","type":"delete_volume","partition_index":{"operation_ref":"first"},"volume_name":"Retained"}
    ]})";
    const auto parsed = axk::parse_alteration_manifest(valid);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->operations.size(), 2U);
    EXPECT_FALSE(axk::parse_alteration_manifest(R"({
    "schema_version":"1.1","operations":[
      {"id":"first","type":"delete_volume","partition_index":{"operation_ref":"later"},"volume_name":"Removed"},
      {"id":"later","type":"delete_volume","partition_index":0,"volume_name":"Retained"}
    ]})"));
}

TEST(AlterationManifestTemplate, EmitsParseableStarterAndPublishesAtomically) {
    const auto serialized = axk::serialize_alteration_manifest_template();
    ASSERT_TRUE(serialized) << serialized.error().message;
    const auto parsed = axk::parse_alteration_manifest(*serialized);
    ASSERT_TRUE(parsed) << parsed.error().message;
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
    "schema_version":"1.1","operations":[
      {"id":"delete","type":"delete_sbnk","partition_index":0,
       "volume_name":"Samples","sample_name":"Old Sample"},
      {"id":"insert","type":"insert_sbnk","partition_index":{"operation_ref":"delete"},
       "volume_name":"Samples","sample":{"name":"New Sample","waveform_name":"Wave",
       "root_key":64,"key_low":10,"key_high":100}}
    ]})");
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto *insert = std::get_if<axk::InsertSampleOperation>(&parsed->operations[1].data);
    ASSERT_NE(insert, nullptr);
    EXPECT_EQ(insert->sample.level, 100U);
    EXPECT_FALSE(axk::parse_alteration_manifest(R"({
    "schema_version":"1.1","operations":[
      {"id":"insert","type":"insert_sbnk","partition_index":0,
       "volume_name":"Samples","sample":{"name":"New Sample","waveform_name":"Wave",
       "root_key":64,"key_low":100,"key_high":10}}
    ]})"));
}

TEST(AlterationManifest, MigratesLegacySampleAndSampleBankFields) {
    const auto parsed = axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"sample","type":"insert_sbnk","partition_index":0,
       "volume_name":"Volume","sample_bank":{"name":"Sample","waveform_name":"Wave",
       "root_key":60,"key_low":0,"key_high":127}},
      {"id":"bank","type":"insert_sbac","partition_index":0,
       "volume_name":"Volume","sample_bank_group":{"name":"Bank","member_sample_banks":["Sample"]}}
    ]})");
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed->schema_version, "1.0");
    const auto *sample = std::get_if<axk::InsertSampleOperation>(&parsed->operations[0].data);
    ASSERT_NE(sample, nullptr);
    EXPECT_EQ(sample->sample.name, "Sample");
    const auto *sample_bank = std::get_if<axk::InsertSampleBankOperation>(&parsed->operations[1].data);
    ASSERT_NE(sample_bank, nullptr);
    EXPECT_EQ(sample_bank->sample_bank.member_samples, std::vector<std::string>{"Sample"});
}

TEST(AlterationManifest, ParsesLanguageNeutralFixtureIntoTypedVariants) {
    const auto path =
        std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/manifests/alteration/all-operations.json";
    const auto parsed = axk::load_alteration_manifest(path);
    ASSERT_TRUE(parsed) << parsed.error().message;
    constexpr std::array expected{
        std::string_view{"delete_volume"},   std::string_view{"insert_volume"},   std::string_view{"delete_sbnk"},
        std::string_view{"insert_sbnk"},     std::string_view{"insert_waveform"}, std::string_view{"delete_waveform"},
        std::string_view{"rename_waveform"}, std::string_view{"rename_sbnk"},     std::string_view{"delete_sbac"},
        std::string_view{"insert_sbac"},     std::string_view{"rename_sbac"},     std::string_view{"delete_program"},
        std::string_view{"insert_program"},  std::string_view{"rename_volume"},   std::string_view{"rename_partition"},
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

TEST(AlterationManifest, ParsesStrictVolumeRename) {
    const auto parsed = axk::parse_alteration_manifest(R"({
      "schema_version":"1.1","operations":[
        {"id":"rename","type":"rename_volume","partition_index":0,
         "volume_name":"Retained","new_volume_name":"Renamed"}
      ]})");
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto *rename = std::get_if<axk::RenameVolumeOperation>(&parsed->operations.front().data);
    ASSERT_NE(rename, nullptr);
    EXPECT_EQ(rename->volume_name, "Retained");
    EXPECT_EQ(rename->new_volume_name, "Renamed");

    EXPECT_FALSE(axk::parse_alteration_manifest(R"({
      "schema_version":"1.1","operations":[
        {"id":"rename","type":"rename_volume","partition_index":0,
         "volume_name":"Retained","new_volume_name":"Renamed","extra":true}
      ]})"));
}

TEST(AlterationManifest, ParsesStrictPartitionRename) {
    const auto parsed = axk::parse_alteration_manifest(R"({
      "schema_version":"1.1","operations":[
        {"id":"rename","type":"rename_partition","partition_index":1,
         "partition_name":"PARTITION 2","new_partition_name":"Samples"}
      ]})");
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto *rename = std::get_if<axk::RenamePartitionOperation>(&parsed->operations.front().data);
    ASSERT_NE(rename, nullptr);
    EXPECT_EQ(rename->partition_name, "PARTITION 2");
    EXPECT_EQ(rename->new_partition_name, "Samples");

    EXPECT_FALSE(axk::parse_alteration_manifest(R"({
      "schema_version":"1.1","operations":[
        {"id":"rename","type":"rename_partition","partition_index":1,
         "partition_name":"PARTITION 2","new_partition_name":"Samples","extra":true}
      ]})"));
    EXPECT_FALSE(axk::parse_alteration_manifest(R"({
      "schema_version":"1.1","operations":[
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
        R"({"schema_version":"1.1","operations":[{"id":"remove","type":"delete_volume","partition_index":0,"volume_name":"Removed"}]})");
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

TEST(Alteration, InsertsFirstVolumeIntoEmptyPartition) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-insert-first-volume";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    axk::HdsBuildManifest source_spec{"1.1", 4U * 1024U * 1024U, {}};
    source_spec.partitions.push_back({"hd1", {}});
    ASSERT_TRUE(axk::write_hds_image(source_spec, source));

    const auto manifest = axk::parse_alteration_manifest(R"({
      "schema_version":"1.1","operations":[
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
      "schema_version":"1.1","operations":[
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
      "schema_version":"1.1","operations":[
        {"id":"rename","type":"rename_volume","partition_index":0,
         "volume_name":"Chain","new_volume_name":"Chain"}
      ]})");
    EXPECT_FALSE(duplicate);
    const auto too_long = axk::parse_alteration_manifest(R"({
      "schema_version":"1.1","operations":[
        {"id":"rename","type":"rename_volume","partition_index":0,
         "volume_name":"Chain","new_volume_name":"This name is too long"}
      ]})");
    EXPECT_FALSE(too_long);
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, RenamePartitionChangesOnlySelectedMirroredHeaderName) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-rename-partition";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    axk::HdsBuildManifest manifest{"1.1", 8U * 1024U * 1024U, {}};
    manifest.partitions.push_back({"PARTITION 1", {}});
    manifest.partitions.push_back({"PARTITION 2", {}});
    ASSERT_TRUE(axk::write_hds_image(manifest, source));
    const auto before_image = axk::open_image(source);
    ASSERT_TRUE(before_image) << before_image.error().message;
    ASSERT_EQ(before_image->partitions().size(), 2U);
    const auto before_bytes = bytes(source);

    const auto alteration = axk::parse_alteration_manifest(R"({
      "schema_version":"1.1","operations":[
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
      "schema_version":"1.1","operations":[
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
        R"({"schema_version":"1.1","operations":[{"id":"remove","type":"delete_volume","partition_index":0,"volume_name":"Removed"}]})");
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
    "schema_version":"1.1","operations":[
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
        R"({"schema_version":"1.1","operations":[{"id":"remove","type":"delete_volume","partition_index":0,"volume_name":"Removed"}]})");
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
        R"({"schema_version":"1.1","operations":[{"id":"delete","type":"delete_volume","partition_index":0,"volume_name":"Removed"}]})");
    ASSERT_TRUE(manifest);
    EXPECT_FALSE(axk::alter_hds(source, *manifest, output));
    EXPECT_FALSE(std::filesystem::exists(output));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, RejectsSharedSampleBankMemberAndNonzeroRenameHandle) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-sample-bank-safety";
    const auto audio = root / "tone.wav";
    const auto source = root / "source.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_wav_atomic(audio, test_waveform()));
    ASSERT_TRUE(axk::write_hds_image(chain_source_manifest(audio), source));
    const auto shared = axk::parse_alteration_manifest(R"({
    "schema_version":"1.1","operations":[
      {"id":"insert","type":"insert_sbac","partition_index":0,"volume_name":"Chain",
       "sample_bank":{"name":"Other Bank","member_samples":["Banked Sample"]}}
    ]})");
    ASSERT_TRUE(shared);
    EXPECT_FALSE(axk::inspect_hds_alteration(source, *shared));

    const auto opened = axk::open_image(source);
    ASSERT_TRUE(opened);
    const auto &partition = opened->partitions()[0];
    const auto program = std::ranges::find_if(partition.records, [](const auto &record) {
        return record.object_type == "PROG" && record.object_name == "033";
    });
    ASSERT_NE(program, partition.records.end());
    patch_record_byte(source, partition, *program, 0x133U, std::byte{1});
    const auto rename = axk::parse_alteration_manifest(R"({
    "schema_version":"1.1","operations":[
      {"id":"rename","type":"rename_sbac","partition_index":0,"volume_name":"Chain",
       "sample_bank_name":"Bank","new_sample_bank_name":"Renamed"}
    ]})");
    ASSERT_TRUE(rename);
    EXPECT_FALSE(axk::inspect_hds_alteration(source, *rename));
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
    "schema_version":"1.1","operations":[
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
    "schema_version":"1.1","operations":[
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
    axk::HdsBuildManifest source_spec{"1.1", 4U * 1024U * 1024U, {}};
    axk::VolumeSpec volume;
    volume.name = "Queue";
    source_spec.partitions.push_back({"hd1", {std::move(volume)}});
    ASSERT_TRUE(axk::write_hds_image(source_spec, source));
    const auto manifest = axk::parse_alteration_manifest(
        R"({"schema_version":"1.1","operations":[
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
    "schema_version":"1.1","operations":[
      {"id":"wave","type":"delete_waveform","partition_index":0,
       "volume_name":"Samples","waveform_name":"Wave"}
    ]})");
    ASSERT_TRUE(rejected);
    EXPECT_FALSE(axk::inspect_hds_alteration(source, *rejected));
    const auto accepted = axk::parse_alteration_manifest(R"({
    "schema_version":"1.1","operations":[
      {"id":"sample","type":"delete_sbnk","partition_index":0,
       "volume_name":"Samples","sample_name":"Old Sample"},
      {"id":"wave","type":"delete_waveform","partition_index":{"operation_ref":"sample"},
       "volume_name":"Samples","waveform_name":"Wave"}
    ]})");
    ASSERT_TRUE(accepted);
    ASSERT_TRUE(axk::alter_hds(source, *accepted, output));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, VolumeDeletionRejectsKnownCrossVolumeWaveformDependency) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-alteration-cross-volume-waveform";
    const auto audio = root / "tone.wav";
    const auto source = root / "source.hds";
    const auto shared = root / "shared.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_wav_atomic(audio, test_waveform()));

    axk::HdsBuildManifest manifest{"1.1", 4U * 1024U * 1024U, {}};
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
    const auto disposable_link = decoded_wave_data->link_id.value + 1U;
    patch_record_be32(source, partition, *wave_data_record_b, 0x6cU, disposable_link - 0xbaU);
    patch_record_be32(source, partition, *wave_data_record_b, 0x78U, disposable_link);
    patch_record_name(source, partition, *record_b, 0x78U, "Shared Wave");
    patch_record_be32(source, partition, *record_b, 0xa0U, decoded_wave_data->link_id.value);

    const auto remove_duplicate = axk::parse_alteration_manifest(R"({
    "schema_version":"1.1","operations":[
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
    const auto cross_volume = std::ranges::find_if(graph.relationships, [&](const auto &relation) {
        if (relation.type != "SBNK_LEFT_MEMBER_TO_SMPL" || relation.quality != axk::RelationshipQuality::known ||
            !relation.target_key)
            return false;
        const auto source_object =
            std::ranges::find(shared_catalog->objects, relation.source_key, &axk::ObjectSnapshot::key);
        const auto target_object =
            std::ranges::find(shared_catalog->objects, *relation.target_key, &axk::ObjectSnapshot::key);
        return source_object != shared_catalog->objects.end() && source_object->placement &&
               source_object->placement->volume_name == "Volume B" && target_object != shared_catalog->objects.end() &&
               target_object->placement && target_object->placement->volume_name == "Volume A";
    });
    ASSERT_NE(cross_volume, graph.relationships.end());

    const auto delete_owner = axk::parse_alteration_manifest(R"({
    "schema_version":"1.1","operations":[
      {"id":"volume","type":"delete_volume","partition_index":0,
       "volume_name":"Volume A"}
    ]})");
    ASSERT_TRUE(delete_owner);
    const auto rejected = axk::inspect_hds_alteration(shared, *delete_owner);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().message, "a known object relationship crosses the volume closure");
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
    axk::HdsBuildManifest source_spec{"1.1", 4U * 1024U * 1024U, {}};
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
    for (std::uint32_t index = 0; index < 48U; ++index) {
        mark_cluster_used(source, partition, first_free + index * 2U + 1U);
    }
    const auto manifest = axk::parse_alteration_manifest(
        R"({"schema_version":"1.1","operations":[
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
