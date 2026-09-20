#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/alteration.hpp"
#include "axklib/audio.hpp"
#include "axklib/bytes.hpp"
#include "axklib/catalog.hpp"
#include "axklib/filesystem_edit.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/writer.hpp"

namespace {
using Json = nlohmann::json;

axk::Result<axk::AlterationManifest> parse_duplicate(const Json &operation) {
    return axk::parse_alteration_manifest(
        Json{{"schema_version", "1.0"}, {"operations", Json::array({operation})}}.dump());
}

std::vector<char> image_bytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
}

axk::Result<axk::ObjectCatalog> catalog(const std::filesystem::path &path) {
    const auto image = axk::open_image(path);
    if (!image)
        return std::unexpected(image.error());
    return axk::build_object_catalog(*image);
}

const axk::ObjectSnapshot *find(const axk::ObjectCatalog &objects, axk::ObjectType type, std::string_view name) {
    const auto found = std::ranges::find_if(objects.objects, [&](const auto &object) {
        return object.object.header.type == type && object.object.header.name == name;
    });
    return found == objects.objects.end() ? nullptr : &*found;
}

void put_name(std::vector<std::byte> &payload, std::size_t offset, std::string_view name) {
    std::fill_n(payload.begin() + static_cast<std::ptrdiff_t>(offset), 16U, std::byte{' '});
    std::ranges::transform(name, payload.begin() + static_cast<std::ptrdiff_t>(offset),
                           [](char value) { return static_cast<std::byte>(value); });
}

class CancelAfterDuplicate final : public axk::ProgressSink {
  public:
    explicit CancelAfterDuplicate(axk::CancellationSource &source) : source_(source) {}

    void report(const axk::Progress &progress) noexcept override {
        if (progress.phase == axk::ProgressPhase::allocating && progress.completed == 1U)
            source_.cancel();
    }

  private:
    axk::CancellationSource &source_;
};

class SampleDuplicate : public testing::Test {
  protected:
    std::filesystem::path root;
    std::filesystem::path source;
    std::filesystem::path output;
    axk::ObjectCatalog before;

    void SetUp() override {
        const auto prefix =
            std::string{"axklib-sample-duplicate-"} + testing::UnitTest::GetInstance()->current_test_info()->name();
        for (std::size_t attempt = 0; attempt < 1024U; ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path() / (prefix + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                root = candidate;
                break;
            }
            ASSERT_FALSE(error) << error.message();
        }
        ASSERT_FALSE(root.empty());
        source = root / "source.hds";
        output = root / "output.hds";
        axk::VolumeSpec volume;
        volume.name = "Samples";
        for (const auto *name : {"Left", "Right"}) {
            axk::Waveform waveform;
            waveform.format = {1U, 2U, 44'100U};
            waveform.frame_count = 16U;
            for (std::uint32_t frame = 0; frame < 16U; ++frame) {
                waveform.pcm.push_back(static_cast<std::byte>(frame));
                waveform.pcm.push_back(std::byte{0x40});
            }
            const auto audio = root / (std::string{name} + ".wav");
            ASSERT_TRUE(axk::write_wav_atomic(audio, waveform));
            volume.waveforms.push_back({name, name, audio, 60U, {}});
        }
        axk::SampleSpec sample;
        sample.name = "Mono";
        sample.waveform_id = "Left";
        sample.playback_window = axk::SamplePlaybackWindow{2U, 12U};
        sample.parameters.level = 83U;
        sample.parameters.loop_mode = axk::AudioSamplerLoopMode::forward_loop;
        sample.parameters.loop_start_frame = 4U;
        sample.parameters.loop_length_frames = 6U;
        volume.samples.push_back(sample);
        sample.name = "Stereo";
        sample.right_waveform_id = "Right";
        volume.samples.push_back(sample);
        sample.name = "Short";
        sample.right_waveform_id.reset();
        volume.samples.push_back(sample);
        volume.sample_banks.push_back({"Bank", {"Mono"}});
        volume.programs.push_back({1U, "Links", {{"SBAC", "Bank", {}}, {"SBNK", "Stereo", {}}}});
        const axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {{"Partition", {volume}}}};
        const auto written = axk::write_hds_image(manifest, source);
        ASSERT_TRUE(written) << written.error().message;
        const auto objects = catalog(source);
        ASSERT_TRUE(objects);
        before = *objects;
        for (const auto *name : {"Mono", "Stereo", "Short"}) {
            const auto *object = find(before, axk::ObjectType::sbnk, name);
            ASSERT_NE(object, nullptr);
            auto payload = object->raw_payload;
            payload[0x43U] = std::byte{0x5a};
            payload[0x98U] = std::byte{0x4b};
            if (std::string_view{name} == "Short") {
                payload.resize(0x164U);
                ASSERT_TRUE(axk::ByteWriter{payload}.write_be32(0x14U, 2U));
                ASSERT_TRUE(axk::ByteWriter{payload}.write_be32(0x18U, 0x134U));
                ASSERT_TRUE(axk::ByteWriter{payload}.write_be32(0x1cU, 0U));
            }
            replace_payload(*object, std::move(payload));
            ASSERT_FALSE(HasFatalFailure());
        }
    }

    void TearDown() override {
        std::error_code error;
        if (!root.empty())
            std::filesystem::remove_all(root, error);
    }

    void replace_payload(const axk::ObjectSnapshot &object, std::vector<std::byte> payload) {
        ASSERT_TRUE(object.placement);
        const auto path = root / ("fixture-" + std::to_string(before.objects.size()) + "-" +
                                  std::to_string(object.sfs_id.value) + ".hds");
        const std::vector<axk::FilesystemEdit> edits{
            axk::PutFilesystemFile{{"Samples", object.placement->category_name, object.placement->entry_name},
                                   std::make_shared<axk::MemoryReader>(std::move(payload)),
                                   axk::FileConflict::replace}};
        const auto replaced = axk::write_sfs_file_edits(source, path, axk::PartitionIndex{0}, edits);
        ASSERT_TRUE(replaced) << replaced.error().message;
        source = path;
        const auto objects = catalog(source);
        ASSERT_TRUE(objects) << objects.error().message;
        before = *objects;
    }

    Json operation(std::string_view source_name = "Mono", std::string_view new_name = "Copy") const {
        const auto *sample = find(before, axk::ObjectType::sbnk, source_name);
        const auto hash = sample ? axk::package_internal::hex_digest(axk::package_internal::sha256(sample->raw_payload))
                                 : std::string(64U, '0');
        return {{"id", "duplicate"},
                {"type", "duplicate_sbnk"},
                {"partition_index", 0},
                {"volume_name", "Samples"},
                {"sample_name", source_name},
                {"new_name", new_name},
                {"parameters", Json::object()},
                {"expected_payload_sha256", hash}};
    }

    void expect_originals_unchanged(const axk::ObjectCatalog &after) const {
        ASSERT_EQ(after.objects.size(), before.objects.size() + 1U);
        for (const auto &old : before.objects) {
            const auto current = std::ranges::find(after.objects, old.key, &axk::ObjectSnapshot::key);
            ASSERT_NE(current, after.objects.end());
            EXPECT_EQ(current->raw_payload, old.raw_payload) << old.object.header.name;
        }
    }

    void expect_rejected(const Json &edit, axk::ErrorCode expected = axk::ErrorCode::transaction_rejected) {
        const auto parsed = parse_duplicate(edit);
        ASSERT_TRUE(parsed) << parsed.error().message;
        const auto original = image_bytes(source);
        const auto applied = axk::alter_hds(source, *parsed, output);
        ASSERT_FALSE(applied);
        EXPECT_EQ(applied.error().code, expected) << applied.error().message;
        EXPECT_FALSE(std::filesystem::exists(output));
        EXPECT_EQ(image_bytes(source), original);
    }
};

TEST_F(SampleDuplicate, ManifestAllowsUneditedCopyAndRejectsMalformedNamesGuardsAndPatches) {
    const auto parsed = parse_duplicate(operation());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(axk::operation_type_name(parsed->operations.front().data), "duplicate_sbnk");
    for (const auto *field : {"sample_name", "new_name"}) {
        for (const auto &name : {std::string{}, std::string(17U, 'a'), std::string{"\ninvalid"}}) {
            auto invalid = operation();
            invalid[field] = name;
            EXPECT_FALSE(parse_duplicate(invalid)) << invalid.dump();
        }
        auto missing = operation();
        missing.erase(field);
        EXPECT_FALSE(parse_duplicate(missing)) << missing.dump();
    }
    for (const auto &parameters : {Json{{"unknown", 1}}, Json{{"level", 128}}, Json{{"level", true}}, Json(nullptr)}) {
        auto invalid = operation();
        invalid["parameters"] = parameters;
        EXPECT_FALSE(parse_duplicate(invalid)) << invalid.dump();
    }
    auto invalid = operation();
    invalid["expected_payload_sha256"] = "ABC";
    EXPECT_FALSE(parse_duplicate(invalid));
    invalid = operation();
    invalid["playback_window"] = {{"start_frame", 0}, {"length_frames", 0}};
    EXPECT_FALSE(parse_duplicate(invalid));
    invalid = operation();
    invalid["copy_wave_data"] = true;
    EXPECT_FALSE(parse_duplicate(invalid));
}

TEST_F(SampleDuplicate, CopiesBankedMonoAssignedStereoAndShortPayloadAsStandaloneWithoutCopyingWaveData) {
    for (const auto *name : {"Mono", "Stereo", "Short"}) {
        SCOPED_TRACE(name);
        const auto *old = find(before, axk::ObjectType::sbnk, name);
        ASSERT_NE(old, nullptr);
        const auto *old_sample = std::get_if<axk::CurrentSbnk>(&old->object.payload);
        ASSERT_NE(old_sample, nullptr);
        if (std::string_view{name} == "Mono")
            ASSERT_EQ(old->raw_payload[0xd0U] & std::byte{1}, std::byte{1});
        if (std::string_view{name} == "Stereo")
            ASSERT_EQ(old_sample->linked_program_numbers, std::vector<std::uint8_t>{1U});
        if (std::string_view{name} == "Short")
            ASSERT_EQ(old->raw_payload.size(), 0x164U);
        const auto parsed = parse_duplicate(operation(name));
        ASSERT_TRUE(parsed) << parsed.error().message;
        const auto original = image_bytes(source);
        const auto destination = root / (std::string{name} + "-copy.hds");
        const auto applied = axk::alter_hds(source, *parsed, destination);
        ASSERT_TRUE(applied) << applied.error().message;
        EXPECT_EQ(image_bytes(source), original);
        const auto after = catalog(destination);
        ASSERT_TRUE(after) << after.error().message;
        expect_originals_unchanged(*after);
        const auto *copy = find(*after, axk::ObjectType::sbnk, "Copy");
        ASSERT_NE(copy, nullptr);
        EXPECT_NE(copy->sfs_id, old->sfs_id);
        ASSERT_TRUE(copy->placement);
        EXPECT_EQ(copy->placement->volume_name, "Samples");
        EXPECT_EQ(copy->placement->entry_name, "Copy");
        auto expected = old->raw_payload;
        put_name(expected, 0x32U, "Copy");
        std::fill(expected.begin() + 0xc0, expected.begin() + 0xd0, std::byte{0});
        expected[0xd0U] &= std::byte{0xfe};
        EXPECT_EQ(copy->raw_payload, expected);
        const auto *sample = std::get_if<axk::CurrentSbnk>(&copy->object.payload);
        ASSERT_NE(sample, nullptr);
        EXPECT_TRUE(sample->linked_program_numbers.empty());
        EXPECT_EQ(sample->left.wave_data_name, "Left");
        EXPECT_EQ(sample->right.has_value(), std::string_view{name} == "Stereo");
        if (sample->right)
            EXPECT_EQ(sample->right->wave_data_name, "Right");
    }
}

TEST_F(SampleDuplicate, AppliesCurrentParameterAndPlaybackEditsOnlyToTheNewSample) {
    for (const auto *name : {"Mono", "Stereo", "Short"}) {
        SCOPED_TRACE(name);
        auto edit = operation(name);
        edit["parameters"] = {{"level", 97}};
        edit["playback_window"] = {{"start_frame", 1}, {"length_frames", 14}};
        const auto parsed = parse_duplicate(edit);
        ASSERT_TRUE(parsed) << parsed.error().message;
        const auto destination = root / (std::string{name} + "-edited.hds");
        const auto original = image_bytes(source);
        const auto applied = axk::alter_hds(source, *parsed, destination);
        ASSERT_TRUE(applied) << applied.error().message;
        EXPECT_EQ(image_bytes(source), original);
        const auto after = catalog(destination);
        ASSERT_TRUE(after);
        expect_originals_unchanged(*after);
        const auto *old = find(before, axk::ObjectType::sbnk, name);
        const auto *copy = find(*after, axk::ObjectType::sbnk, "Copy");
        ASSERT_NE(old, nullptr);
        ASSERT_NE(copy, nullptr);
        auto expected = old->raw_payload;
        put_name(expected, 0x32U, "Copy");
        std::fill(expected.begin() + 0xc0, expected.begin() + 0xd0, std::byte{0});
        expected[0xd0U] &= std::byte{0xfe};
        expected[0x116U] = std::byte{97};
        axk::ByteWriter writer{expected};
        ASSERT_TRUE(writer.write_be32(0xe8U, 1U));
        ASSERT_TRUE(writer.write_be32(0xf0U, 14U));
        ASSERT_TRUE(writer.write_be32(0x15cU, 15U));
        if (std::string_view{name} == "Stereo") {
            ASSERT_TRUE(writer.write_be32(0xecU, 1U));
            ASSERT_TRUE(writer.write_be32(0xf4U, 14U));
        }
        EXPECT_EQ(copy->raw_payload, expected);
    }
}

TEST_F(SampleDuplicate, RejectsCaseInsensitiveCollisionsIncludingTheSourceNameWithoutPublishingChanges) {
    for (const auto *name : {"Mono", "mono", "mOnO", "STEREO", "Short"}) {
        SCOPED_TRACE(name);
        expect_rejected(operation("Mono", name));
    }
}

TEST_F(SampleDuplicate, RejectsStaleSourceHashWithoutPublishingChanges) {
    auto edit = operation();
    edit["expected_payload_sha256"] = std::string(64U, '0');
    expect_rejected(edit, axk::ErrorCode::transaction_stale);
}

TEST_F(SampleDuplicate, InvalidMergedPlaybackBoundsPreserveExistingDestinationAndSource) {
    const auto *object = find(before, axk::ObjectType::sbnk, "Stereo");
    ASSERT_NE(object, nullptr);
    const auto *sample = std::get_if<axk::CurrentSbnk>(&object->object.payload);
    ASSERT_NE(sample, nullptr);
    ASSERT_EQ(sample->loop_mode, 1U);
    ASSERT_EQ(sample->left.loop_start_frame, 4U);
    ASSERT_EQ(sample->left.loop_length_frames, 6U);
    auto edit = operation("Stereo");
    edit["playback_window"] = {{"start_frame", 1}, {"length_frames", 2}};
    const auto parsed = parse_duplicate(edit);
    ASSERT_TRUE(parsed) << parsed.error().message;
    std::filesystem::copy_file(source, output);
    const auto original = image_bytes(source);
    const auto destination = image_bytes(output);
    EXPECT_FALSE(axk::alter_hds(source, *parsed, output, {}, nullptr, true));
    EXPECT_EQ(image_bytes(source), original);
    EXPECT_EQ(image_bytes(output), destination);
}

TEST_F(SampleDuplicate, RejectsDestinationNameAlreadyReferencedBySampleBank) {
    const auto *object = find(before, axk::ObjectType::sbac, "Bank");
    ASSERT_NE(object, nullptr);
    const auto *bank = std::get_if<axk::CurrentSbac>(&object->object.payload);
    ASSERT_NE(bank, nullptr);
    ASSERT_FALSE(bank->slots.empty());
    auto payload = object->raw_payload;
    put_name(payload, bank->slots.front().offset, "cOpY");
    replace_payload(*object, std::move(payload));
    ASSERT_FALSE(HasFatalFailure());
    expect_rejected(operation("Short"));
}

TEST_F(SampleDuplicate, RejectsDestinationNameAlreadyReferencedByProgram) {
    const auto object = std::ranges::find_if(before.objects, [](const auto &value) {
        return value.object.header.type == axk::ObjectType::prog && value.placement &&
               value.placement->entry_name == "001";
    });
    ASSERT_NE(object, before.objects.end());
    const auto *program = std::get_if<axk::CurrentProg>(&object->object.payload);
    ASSERT_NE(program, nullptr);
    const auto assignment = std::ranges::find_if(
        program->assignments, [](const auto &value) { return value.kind == 0x10U && value.name == "Stereo"; });
    ASSERT_NE(assignment, program->assignments.end());
    auto payload = object->raw_payload;
    put_name(payload, assignment->offset, "cOpY");
    replace_payload(*object, std::move(payload));
    ASSERT_FALSE(HasFatalFailure());
    expect_rejected(operation("Short"));
}

TEST_F(SampleDuplicate, CancellationDoesNotPublishDuplicate) {
    const auto parsed = parse_duplicate(operation());
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto original = image_bytes(source);
    axk::CancellationSource cancellation;
    cancellation.cancel();
    const auto applied = axk::alter_hds(source, *parsed, output, cancellation.token());
    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().code, axk::ErrorCode::operation_cancelled);
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_EQ(image_bytes(source), original);
}

TEST_F(SampleDuplicate, CancellationAfterPreparingTheDuplicateDoesNotPublishChanges) {
    const auto parsed = parse_duplicate(operation());
    ASSERT_TRUE(parsed) << parsed.error().message;
    std::filesystem::copy_file(source, output);
    const auto original = image_bytes(source);
    const auto destination = image_bytes(output);
    axk::CancellationSource cancellation;
    CancelAfterDuplicate progress{cancellation};
    const auto applied = axk::alter_hds(source, *parsed, output, cancellation.token(), &progress, true);
    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().code, axk::ErrorCode::operation_cancelled);
    EXPECT_EQ(image_bytes(source), original);
    EXPECT_EQ(image_bytes(output), destination);
}

TEST_F(SampleDuplicate, ExhaustedSpaceRollsBackEveryQueuedDuplicateAndPreservesExistingDestination) {
    const auto image = axk::open_image(source);
    ASSERT_TRUE(image);
    const auto &partition = image->partitions().front();
    ASSERT_TRUE(partition.allocation.free_space);
    const auto free_clusters = partition.allocation.free_space->free_cluster_count;
    ASSERT_GT(free_clusters, 4U);
    const auto filler = std::make_shared<axk::MemoryReader>(
        std::vector<std::byte>(static_cast<std::size_t>(free_clusters - 4U) * 1024U, std::byte{0x5a}));
    const std::vector<axk::FilesystemEdit> fill{axk::PutFilesystemFile{{"Filler"}, filler}};
    const auto packed = root / "packed.hds";
    const auto filled = axk::write_sfs_file_edits(source, packed, axk::PartitionIndex{0}, fill);
    ASSERT_TRUE(filled) << filled.error().message;
    const auto packed_image = axk::open_image(packed);
    ASSERT_TRUE(packed_image);
    ASSERT_TRUE(packed_image->partitions().front().allocation.free_space);
    ASSERT_LT(packed_image->partitions().front().allocation.free_space->free_cluster_count, 10U);
    auto operations = Json::array();
    for (unsigned index = 0; index < 10U; ++index) {
        auto edit = operation("Mono", "Copy" + std::to_string(index));
        edit["id"] = "duplicate" + std::to_string(index);
        operations.push_back(std::move(edit));
    }
    const auto parsed =
        axk::parse_alteration_manifest(Json{{"schema_version", "1.0"}, {"operations", std::move(operations)}}.dump());
    ASSERT_TRUE(parsed) << parsed.error().message;
    std::filesystem::copy_file(source, output);
    const auto original = image_bytes(packed);
    const auto destination = image_bytes(output);
    const auto applied = axk::alter_hds(packed, *parsed, output, {}, nullptr, true);
    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().category, axk::ErrorCategory::transaction) << applied.error().message;
    EXPECT_EQ(image_bytes(packed), original);
    EXPECT_EQ(image_bytes(output), destination);
}
} // namespace
