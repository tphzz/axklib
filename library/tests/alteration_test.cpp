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
    axk::HdsBuildManifest result{"1.0", 4U * 1024U * 1024U, {}};
    axk::VolumeSpec retained;
    retained.name = "Retained";
    axk::VolumeSpec removed;
    removed.name = "Removed";
    result.partitions.push_back(
        {"hd1", {std::move(retained), std::move(removed)}});
    return result;
}

axk::HdsBuildManifest
bank_source_manifest(const std::filesystem::path &audio_path) {
    axk::HdsBuildManifest result{"1.0", 4U * 1024U * 1024U, {}};
    axk::VolumeSpec volume;
    volume.name = "Banks";
    volume.waveforms.push_back({"wave", "Wave", audio_path, 60U, {}});
    axk::SampleBankSpec bank;
    bank.name = "Old Bank";
    bank.waveform_id = "wave";
    bank.root_key = 60U;
    bank.key_low = 12U;
    bank.key_high = 108U;
    bank.level = 99U;
    volume.sample_banks.push_back(std::move(bank));
    result.partitions.push_back({"hd1", {std::move(volume)}});
    return result;
}

axk::HdsBuildManifest
chain_source_manifest(const std::filesystem::path &audio_path) {
    auto result = bank_source_manifest(audio_path);
    auto &volume = result.partitions[0].volumes[0];
    volume.name = "Chain";
    volume.sample_banks[0].name = "Grouped";
    auto direct = volume.sample_banks[0];
    direct.name = "Direct";
    volume.sample_banks.push_back(std::move(direct));
    volume.sample_bank_groups.push_back({"Group", {"Grouped"}});
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
    result.pcm = {std::byte{0}, std::byte{0},    std::byte{0xe8},
                  std::byte{3}, std::byte{0x18}, std::byte{0xfc},
                  std::byte{0}, std::byte{0}};
    return result;
}

class CancellingProgress final : public axk::ProgressSink {
  public:
    CancellingProgress(axk::CancellationSource &source,
                       std::uint64_t cancel_after)
        : source_(source), cancel_after_(cancel_after) {}

    void report(const axk::Progress &progress) noexcept override {
        if (progress.phase == axk::ProgressPhase::writing &&
            progress.completed == cancel_after_) {
            source_.cancel();
        }
    }

  private:
    axk::CancellationSource &source_;
    std::uint64_t cancel_after_{};
};

void mark_cluster_used(const std::filesystem::path &path,
                       const axk::Partition &partition, std::uint32_t cluster) {
    std::fstream image{path, std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(image);
    const auto byte_index = cluster / 8U;
    const auto mask = static_cast<unsigned char>(0x80U >> (cluster % 8U));
    const std::array offsets{
        (static_cast<std::uint64_t>(partition.start_sector) +
         static_cast<std::uint64_t>(partition.bitmap_cluster) *
             partition.sectors_per_cluster) *
                512U +
            byte_index,
        static_cast<std::uint64_t>(partition.start_sector) * 512U + 2048U +
            byte_index,
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

void mark_cluster_free(const std::filesystem::path &path,
                       const axk::Partition &partition, std::uint32_t cluster) {
    std::fstream image{path, std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(image);
    const auto byte_index = cluster / 8U;
    const auto mask = static_cast<unsigned char>(0x80U >> (cluster % 8U));
    const std::array offsets{
        (static_cast<std::uint64_t>(partition.start_sector) +
         static_cast<std::uint64_t>(partition.bitmap_cluster) *
             partition.sectors_per_cluster) *
                512U +
            byte_index,
        static_cast<std::uint64_t>(partition.start_sector) * 512U + 2048U +
            byte_index,
    };
    for (const auto offset : offsets) {
        image.seekg(static_cast<std::streamoff>(offset));
        char value{};
        image.read(&value, 1);
        ASSERT_TRUE(image);
        value = static_cast<char>(static_cast<unsigned char>(value) &
                                  static_cast<unsigned char>(~mask));
        image.seekp(static_cast<std::streamoff>(offset));
        image.write(&value, 1);
        ASSERT_TRUE(image);
    }
}

void patch_record_byte(const std::filesystem::path &path,
                       const axk::Partition &partition,
                       const axk::IndexRecord &record,
                       std::size_t payload_offset, std::byte value) {
    ASSERT_EQ(record.extents.size(), 1U);
    const auto absolute =
        (static_cast<std::uint64_t>(partition.start_sector) +
         static_cast<std::uint64_t>(record.extents[0].cluster_offset) *
             partition.sectors_per_cluster) *
            512U +
        payload_offset;
    std::fstream image{path, std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(image);
    image.seekp(static_cast<std::streamoff>(absolute));
    image.put(static_cast<char>(value));
    ASSERT_TRUE(image);
}

void patch_record_be32(const std::filesystem::path &path,
                       const axk::Partition &partition,
                       const axk::IndexRecord &record,
                       std::size_t payload_offset, std::uint32_t value) {
    patch_record_byte(path, partition, record, payload_offset,
                      static_cast<std::byte>((value >> 24U) & 0xffU));
    patch_record_byte(path, partition, record, payload_offset + 1U,
                      static_cast<std::byte>((value >> 16U) & 0xffU));
    patch_record_byte(path, partition, record, payload_offset + 2U,
                      static_cast<std::byte>((value >> 8U) & 0xffU));
    patch_record_byte(path, partition, record, payload_offset + 3U,
                      static_cast<std::byte>(value & 0xffU));
}

void patch_record_name(const std::filesystem::path &path,
                       const axk::Partition &partition,
                       const axk::IndexRecord &record,
                       std::size_t payload_offset, std::string_view name) {
    ASSERT_LE(name.size(), 16U);
    for (std::size_t index = 0; index < 16U; ++index) {
        patch_record_byte(path, partition, record, payload_offset + index,
                          index < name.size()
                              ? static_cast<std::byte>(name[index])
                              : std::byte{' '});
    }
}

} // namespace

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

TEST(AlterationManifestTemplate, EmitsParseableStarterAndPublishesAtomically) {
    const auto serialized = axk::serialize_alteration_manifest_template();
    ASSERT_TRUE(serialized) << serialized.error().message;
    const auto parsed = axk::parse_alteration_manifest(*serialized);
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed->operations.size(), 1U);
    EXPECT_EQ(axk::operation_type_name(parsed->operations.front().data),
              "rename_waveform");

    const auto root = std::filesystem::temp_directory_path() /
                      "axklib-alteration-manifest-template-test";
    const auto path = root / "nested" / "transaction.json";
    std::error_code error;
    std::filesystem::remove_all(root, error);

    ASSERT_TRUE(axk::write_alteration_manifest_template(path));
    EXPECT_FALSE(axk::write_alteration_manifest_template(path));
    ASSERT_TRUE(axk::write_alteration_manifest_template(path, true));
    ASSERT_TRUE(axk::load_alteration_manifest(path));

    std::filesystem::remove_all(root, error);
}

TEST(AlterationManifest, ParsesStrictSampleBankOperations) {
    const auto parsed = axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"delete","type":"delete_sbnk","partition_index":0,
       "volume_name":"Banks","sample_bank_name":"Old Bank"},
      {"id":"insert","type":"insert_sbnk","partition_index":{"operation_ref":"delete"},
       "volume_name":"Banks","sample_bank":{"name":"New Bank","waveform_name":"Wave",
       "root_key":64,"key_low":10,"key_high":100}}
    ]})");
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto *insert = std::get_if<axk::InsertSampleBankOperation>(
        &parsed->operations[1].data);
    ASSERT_NE(insert, nullptr);
    EXPECT_EQ(insert->sample_bank.level, 100U);
    EXPECT_FALSE(axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"insert","type":"insert_sbnk","partition_index":0,
       "volume_name":"Banks","sample_bank":{"name":"New Bank","waveform_name":"Wave",
       "root_key":64,"key_low":100,"key_high":10}}
    ]})"));
}

TEST(AlterationManifest, ParsesLanguageNeutralFixtureIntoTypedVariants) {
    const auto path = std::filesystem::path{AXK_SOURCE_ROOT} /
                      "tests/fixtures/manifests/alteration/all-operations.json";
    const auto parsed = axk::load_alteration_manifest(path);
    ASSERT_TRUE(parsed) << parsed.error().message;
    constexpr std::array expected{
        std::string_view{"delete_volume"},
        std::string_view{"insert_volume"},
        std::string_view{"delete_sbnk"},
        std::string_view{"insert_sbnk"},
        std::string_view{"insert_waveform"},
        std::string_view{"delete_waveform"},
        std::string_view{"rename_waveform"},
        std::string_view{"rename_sbnk"},
        std::string_view{"delete_sbac"},
        std::string_view{"insert_sbac"},
        std::string_view{"rename_sbac"},
        std::string_view{"delete_program"},
        std::string_view{"insert_program"},
    };
    ASSERT_EQ(parsed->operations.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(axk::operation_type_name(parsed->operations[index].data),
                  expected[index]);
        EXPECT_EQ(parsed->operations[index].data.index(), index);
    }
    const auto *deleted =
        std::get_if<axk::DeleteProgramOperation>(&parsed->operations[11].data);
    ASSERT_NE(deleted, nullptr);
    EXPECT_EQ(deleted->program_number, 128U);
}

TEST(Alteration, DeleteVolumeDryRunMatchesApplyAndPreservesSource) {
    const auto root =
        std::filesystem::temp_directory_path() / "axklib-alteration-test";
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
    const auto planned = axk::alter_hds(source, *manifest);
    ASSERT_TRUE(planned);
    ASSERT_EQ(planned->operations.size(), 1U);
    EXPECT_EQ(planned->operations[0].freed_clusters, 12U);
    const auto applied = axk::alter_hds(source, *manifest, output);
    ASSERT_TRUE(applied);
    EXPECT_EQ(applied->operations[0].removed_sfs_ids,
              planned->operations[0].removed_sfs_ids);
    EXPECT_EQ(bytes(source), source_before);
    const auto reopened = axk::open_image(output);
    ASSERT_TRUE(reopened);
    const auto &root_record =
        *std::ranges::find(reopened->partitions()[0].records, axk::SfsId{1},
                           &axk::IndexRecord::sfs_id);
    EXPECT_TRUE(std::ranges::any_of(
        root_record.directory_entries,
        [](const auto &entry) { return entry.name == "Retained"; }));
    EXPECT_FALSE(std::ranges::any_of(
        root_record.directory_entries,
        [](const auto &entry) { return entry.name == "Removed"; }));
    EXPECT_FALSE(axk::alter_hds(source, *manifest, output));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, CancellationPublishesNothing) {
    const auto root =
        std::filesystem::temp_directory_path() / "axklib-alteration-cancel";
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
    EXPECT_FALSE(
        axk::alter_hds(source, *manifest, output, cancellation.token()));
    EXPECT_FALSE(std::filesystem::exists(output));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, CancellationAfterEveryQueuedOperationPublishesNothing) {
    const auto root = std::filesystem::temp_directory_path() /
                      "axklib-alteration-cancel-queue";
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
        const auto result = axk::alter_hds(source, *manifest, output,
                                           cancellation.token(), &progress);
        EXPECT_FALSE(result);
        EXPECT_EQ(result.error().code, axk::ErrorCode::operation_cancelled);
        EXPECT_FALSE(std::filesystem::exists(output));
        EXPECT_EQ(bytes(source), source_before);
    }
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, ConcurrentPublishersUseUniqueTemporarySiblings) {
    const auto root =
        std::filesystem::temp_directory_path() / "axklib-alteration-concurrent";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_hds_image(source_manifest(), source));
    const auto manifest = axk::parse_alteration_manifest(
        R"({"schema_version":"1.0","operations":[{"id":"remove","type":"delete_volume","partition_index":0,"volume_name":"Removed"}]})");
    ASSERT_TRUE(manifest);
    auto first = std::async(std::launch::async, [&] {
        return axk::alter_hds(source, *manifest, output).has_value();
    });
    auto second = std::async(std::launch::async, [&] {
        return axk::alter_hds(source, *manifest, output).has_value();
    });
    EXPECT_NE(first.get(), second.get());
    EXPECT_TRUE(std::filesystem::exists(output));
    EXPECT_FALSE(std::ranges::any_of(
        std::filesystem::directory_iterator{root}, [](const auto &entry) {
            return entry.path().filename().string().starts_with(
                ".output.hds.alter.");
        }));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, RejectsSourceBitmapThatExposesLiveExtentsAsFree) {
    const auto root = std::filesystem::temp_directory_path() /
                      "axklib-alteration-stale-bitmap";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_hds_image(source_manifest(), source));
    const auto opened = axk::open_image(source);
    ASSERT_TRUE(opened);
    const auto &partition = opened->partitions()[0];
    const auto live =
        std::ranges::find_if(partition.records, [](const auto &record) {
            return record.sfs_id.value >= 3U && !record.extents.empty();
        });
    ASSERT_NE(live, partition.records.end());
    mark_cluster_free(source, partition, live->extents[0].cluster_offset);
    const auto manifest = axk::parse_alteration_manifest(
        R"({"schema_version":"1.0","operations":[{"id":"delete","type":"delete_volume","partition_index":0,"volume_name":"Removed"}]})");
    ASSERT_TRUE(manifest);
    EXPECT_FALSE(axk::alter_hds(source, *manifest, output));
    EXPECT_FALSE(std::filesystem::exists(output));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, RejectsSharedGroupMemberAndNonzeroRenameHandle) {
    const auto root = std::filesystem::temp_directory_path() /
                      "axklib-alteration-group-safety";
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
       "sample_bank_group":{"name":"Other Group","member_sample_banks":["Grouped"]}}
    ]})");
    ASSERT_TRUE(shared);
    EXPECT_FALSE(axk::alter_hds(source, *shared));

    const auto opened = axk::open_image(source);
    ASSERT_TRUE(opened);
    const auto &partition = opened->partitions()[0];
    const auto program =
        std::ranges::find_if(partition.records, [](const auto &record) {
            return record.object_type == "PROG" && record.object_name == "033";
        });
    ASSERT_NE(program, partition.records.end());
    patch_record_byte(source, partition, *program, 0x133U, std::byte{1});
    const auto rename = axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"rename","type":"rename_sbac","partition_index":0,"volume_name":"Chain",
       "sample_bank_group_name":"Group","new_sample_bank_group_name":"Renamed"}
    ]})");
    ASSERT_TRUE(rename);
    EXPECT_FALSE(axk::alter_hds(source, *rename));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, QueueReusesDeletedIdsAndAllocationForInsertedVolume) {
    const auto root =
        std::filesystem::temp_directory_path() / "axklib-alteration-queue";
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
       "volume":{"name":"Replacement","waveforms":[],"sample_banks":[]}}
    ]})");
    ASSERT_TRUE(manifest) << manifest.error().message;
    const auto applied = axk::alter_hds(source, *manifest, output);
    ASSERT_TRUE(applied) << applied.error().message;
    ASSERT_EQ(applied->operations.size(), 2U);
    EXPECT_EQ(applied->operations[0].removed_sfs_ids,
              applied->operations[1].inserted_sfs_ids);
    EXPECT_EQ(applied->operations[0].freed_clusters,
              applied->operations[1].allocated_clusters);
    const auto reopened = axk::open_image(output);
    ASSERT_TRUE(reopened);
    const auto &root_record =
        *std::ranges::find(reopened->partitions()[0].records, axk::SfsId{1},
                           &axk::IndexRecord::sfs_id);
    EXPECT_TRUE(std::ranges::any_of(
        root_record.directory_entries,
        [](const auto &entry) { return entry.name == "Replacement"; }));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, DeleteThenInsertSampleBankReusesRecordAndAllocation) {
    const auto root =
        std::filesystem::temp_directory_path() / "axklib-alteration-sbnk";
    const auto audio = root / "tone.wav";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_wav_atomic(audio, test_waveform()));
    ASSERT_TRUE(axk::write_hds_image(bank_source_manifest(audio), source));
    const auto manifest = axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"delete","type":"delete_sbnk","partition_index":0,
       "volume_name":"Banks","sample_bank_name":"Old Bank"},
      {"id":"insert","type":"insert_sbnk","partition_index":{"operation_ref":"delete"},
       "volume_name":"Banks","sample_bank":{"name":"New Bank","waveform_name":"Wave",
       "root_key":64,"key_low":10,"key_high":100,"level":87}}
    ]})");
    ASSERT_TRUE(manifest) << manifest.error().message;
    const auto applied = axk::alter_hds(source, *manifest, output);
    ASSERT_TRUE(applied) << applied.error().message;
    ASSERT_EQ(applied->operations.size(), 2U);
    EXPECT_EQ(applied->operations[0].removed_sfs_ids,
              applied->operations[1].inserted_sfs_ids);
    EXPECT_EQ(applied->operations[0].freed_clusters,
              applied->operations[1].allocated_clusters);
    const auto reopened = axk::open_image(output);
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(catalog) << catalog.error().message;
    const auto bank =
        std::ranges::find_if(catalog->objects, [](const auto &object) {
            return object.placement &&
                   object.placement->entry_name == "New Bank";
        });
    ASSERT_NE(bank, catalog->objects.end());
    const auto *decoded = std::get_if<axk::CurrentSbnk>(&bank->object.payload);
    ASSERT_NE(decoded, nullptr);
    EXPECT_EQ(decoded->left.sample_name, "Wave");
    EXPECT_EQ(decoded->left.root_key, 64U);
    EXPECT_EQ(decoded->key_range_low, 10U);
    EXPECT_EQ(decoded->key_range_high, 100U);
    EXPECT_EQ(decoded->sample_level, 87U);
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, QueuedWaveformAndSampleBankInsertionUsesEvolvingState) {
    const auto root =
        std::filesystem::temp_directory_path() / "axklib-alteration-wave-queue";
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
        {"id":"bank","type":"insert_sbnk","partition_index":{"operation_ref":"wave"},
         "volume_name":"Queue","sample_bank":{"name":"Bank","waveform_name":"Wave",
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
    const auto waveform =
        std::ranges::find_if(catalog->objects, [](const auto &object) {
            return object.object.header.type == axk::ObjectType::smpl &&
                   object.object.header.name == "Wave";
        });
    ASSERT_NE(waveform, catalog->objects.end());
    const auto decoded_waveform = axk::decode_waveform(*reopened, *waveform);
    ASSERT_TRUE(decoded_waveform) << decoded_waveform.error().message;
    auto expected_stored_pcm = expected_waveform.pcm;
    expected_stored_pcm.insert(expected_stored_pcm.end(),
                               expected_waveform.pcm.begin(),
                               expected_waveform.pcm.end());
    EXPECT_EQ(decoded_waveform->pcm, expected_stored_pcm);
    EXPECT_TRUE(std::ranges::any_of(catalog->objects, [](const auto &object) {
        return object.object.header.type == axk::ObjectType::sbnk &&
               object.object.header.name == "Bank";
    }));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, WaveformDeletionRequiresPriorSampleBankDeletion) {
    const auto root = std::filesystem::temp_directory_path() /
                      "axklib-alteration-wave-delete";
    const auto audio = root / "tone.wav";
    const auto source = root / "source.hds";
    const auto output = root / "output.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    ASSERT_TRUE(axk::write_wav_atomic(audio, test_waveform()));
    ASSERT_TRUE(axk::write_hds_image(bank_source_manifest(audio), source));
    const auto rejected = axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"wave","type":"delete_waveform","partition_index":0,
       "volume_name":"Banks","waveform_name":"Wave"}
    ]})");
    ASSERT_TRUE(rejected);
    EXPECT_FALSE(axk::alter_hds(source, *rejected));
    const auto accepted = axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"bank","type":"delete_sbnk","partition_index":0,
       "volume_name":"Banks","sample_bank_name":"Old Bank"},
      {"id":"wave","type":"delete_waveform","partition_index":{"operation_ref":"bank"},
       "volume_name":"Banks","waveform_name":"Wave"}
    ]})");
    ASSERT_TRUE(accepted);
    ASSERT_TRUE(axk::alter_hds(source, *accepted, output));
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, VolumeDeletionRejectsKnownCrossVolumeWaveformDependency) {
    const auto root = std::filesystem::temp_directory_path() /
                      "axklib-alteration-cross-volume-waveform";
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
    axk::SampleBankSpec bank_a;
    bank_a.name = "Bank A";
    bank_a.waveform_id = "shared-a";
    bank_a.root_key = 60U;
    bank_a.key_high = 127U;
    volume_a.sample_banks.push_back(std::move(bank_a));
    axk::VolumeSpec volume_b;
    volume_b.name = "Volume B";
    volume_b.waveforms.push_back({"unused-b", "Unused Wave", audio, 60U, {}});
    axk::SampleBankSpec bank_b_spec;
    bank_b_spec.name = "Bank B";
    bank_b_spec.waveform_id = "unused-b";
    bank_b_spec.root_key = 67U;
    bank_b_spec.key_high = 127U;
    volume_b.sample_banks.push_back(std::move(bank_b_spec));
    manifest.partitions.push_back(
        {"hd1", {std::move(volume_a), std::move(volume_b)}});
    ASSERT_TRUE(axk::write_hds_image(manifest, source));

    const auto container = axk::open_image(source);
    ASSERT_TRUE(container) << container.error().message;
    const auto catalog = axk::build_object_catalog(*container);
    ASSERT_TRUE(catalog) << catalog.error().message;
    const auto sample_a =
        std::ranges::find_if(catalog->objects, [](const auto &object) {
            return object.placement &&
                   object.placement->volume_name == "Volume A" &&
                   object.object.header.type == axk::ObjectType::smpl &&
                   object.object.header.name == "Shared Wave";
        });
    const auto bank_b =
        std::ranges::find_if(catalog->objects, [](const auto &object) {
            return object.placement &&
                   object.placement->volume_name == "Volume B" &&
                   object.object.header.type == axk::ObjectType::sbnk &&
                   object.object.header.name == "Bank B";
        });
    const auto sample_b =
        std::ranges::find_if(catalog->objects, [](const auto &object) {
            return object.placement &&
                   object.placement->volume_name == "Volume B" &&
                   object.object.header.type == axk::ObjectType::smpl &&
                   object.object.header.name == "Unused Wave";
        });
    ASSERT_NE(sample_a, catalog->objects.end());
    ASSERT_NE(bank_b, catalog->objects.end());
    ASSERT_NE(sample_b, catalog->objects.end());
    const auto *decoded_sample =
        std::get_if<axk::CurrentSmpl>(&sample_a->object.payload);
    ASSERT_NE(decoded_sample, nullptr);
    const auto &partition = container->partitions().front();
    const auto record_b = std::ranges::find(partition.records, bank_b->sfs_id,
                                            &axk::IndexRecord::sfs_id);
    const auto sample_record_b = std::ranges::find(
        partition.records, sample_b->sfs_id, &axk::IndexRecord::sfs_id);
    ASSERT_NE(record_b, partition.records.end());
    ASSERT_NE(sample_record_b, partition.records.end());
    const auto disposable_link = decoded_sample->link_id.value + 1U;
    patch_record_be32(source, partition, *sample_record_b, 0x6cU,
                      disposable_link - 0xbaU);
    patch_record_be32(source, partition, *sample_record_b, 0x78U,
                      disposable_link);
    patch_record_name(source, partition, *record_b, 0x78U, "Shared Wave");
    patch_record_be32(source, partition, *record_b, 0xa0U,
                      decoded_sample->link_id.value);

    const auto remove_duplicate = axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"wave","type":"delete_waveform","partition_index":0,
       "volume_name":"Volume B","waveform_name":"Unused Wave"}
    ]})");
    ASSERT_TRUE(remove_duplicate);
    const auto removed_duplicate =
        axk::alter_hds(source, *remove_duplicate, shared);
    ASSERT_TRUE(removed_duplicate) << removed_duplicate.error().message;

    const auto reopened = axk::open_image(shared);
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto shared_catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(shared_catalog) << shared_catalog.error().message;
    EXPECT_EQ(std::ranges::count_if(shared_catalog->objects,
                                    [](const auto &object) {
                                        return object.object.header.type ==
                                                   axk::ObjectType::smpl &&
                                               object.object.header.name ==
                                                   "Shared Wave";
                                    }),
              1);
    const auto graph = axk::build_relationship_graph(*shared_catalog);
    const auto cross_volume =
        std::ranges::find_if(graph.relationships, [&](const auto &relation) {
            if (relation.type != "SBNK_LEFT_MEMBER_TO_SMPL" ||
                relation.quality != axk::RelationshipQuality::known ||
                !relation.target_key)
                return false;
            const auto source_object =
                std::ranges::find(shared_catalog->objects, relation.source_key,
                                  &axk::ObjectSnapshot::key);
            const auto target_object =
                std::ranges::find(shared_catalog->objects, *relation.target_key,
                                  &axk::ObjectSnapshot::key);
            return source_object != shared_catalog->objects.end() &&
                   source_object->placement &&
                   source_object->placement->volume_name == "Volume B" &&
                   target_object != shared_catalog->objects.end() &&
                   target_object->placement &&
                   target_object->placement->volume_name == "Volume A";
        });
    ASSERT_NE(cross_volume, graph.relationships.end());

    const auto delete_owner = axk::parse_alteration_manifest(R"({
    "schema_version":"1.0","operations":[
      {"id":"volume","type":"delete_volume","partition_index":0,
       "volume_name":"Volume A"}
    ]})");
    ASSERT_TRUE(delete_owner);
    const auto rejected = axk::alter_hds(shared, *delete_owner);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().message,
              "a known object relationship crosses the volume closure");
    std::filesystem::remove_all(root, error);
}

TEST(Alteration, WritesAndReopensFortyEightExtentContinuationList) {
    const auto root =
        std::filesystem::temp_directory_path() / "axklib-alteration-fragmented";
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
    auto first_free = partition.directory_index_cluster +
                      partition.directory_index_span_clusters;
    while (std::ranges::any_of(partition.records, [&](const auto &record) {
        return std::ranges::any_of(record.extents, [&](const auto &extent) {
            return first_free >= extent.cluster_offset &&
                   first_free < extent.cluster_offset + extent.cluster_count;
        });
    })) {
        ++first_free;
    }
    for (std::uint32_t index = 0; index < 48U; ++index) {
        mark_cluster_used(source, partition, first_free + index * 2U + 1U);
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
    const auto inserted = std::ranges::find_if(records, [](const auto &record) {
        return record.object_name == "Large Wave";
    });
    ASSERT_NE(inserted, records.end());
    EXPECT_EQ(inserted->extents.size(), 48U);
    EXPECT_EQ(inserted->continuation_clusters.size(), 1U);
    std::filesystem::remove_all(root, error);
}
