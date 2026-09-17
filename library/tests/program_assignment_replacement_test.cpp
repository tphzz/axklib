#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
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
#include "axklib/catalog.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/writer.hpp"
#include "axklib/writer_internal.hpp"

namespace {
using Json = nlohmann::json;

Json replacement(std::string hash, Json rows) {
    return {{"id", "replace"},
            {"type", "replace_program_assignments"},
            {"partition_index", 0},
            {"volume_name", "Programs"},
            {"program_number", 1},
            {"model", "A4000"},
            {"expected_payload_sha256", std::move(hash)},
            {"assignments", std::move(rows)}};
}

axk::Result<axk::AlterationManifest> parse_replacement(const Json &operation) {
    return axk::parse_alteration_manifest(
        Json{{"schema_version", "1.0"}, {"operations", Json::array({operation})}}.dump());
}

std::vector<char> read_image(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
}

const axk::ObjectSnapshot *find_object(const axk::ObjectCatalog &catalog, axk::ObjectType type, std::string_view name) {
    const auto found = std::ranges::find_if(catalog.objects, [&](const auto &object) {
        return object.object.header.type == type && object.object.header.name == name;
    });
    return found == catalog.objects.end() ? nullptr : &*found;
}

class CancelAfterReplacement final : public axk::ProgressSink {
  public:
    explicit CancelAfterReplacement(axk::CancellationSource &source) : source_(source) {}

    void report(const axk::Progress &progress) noexcept override {
        if (progress.phase == axk::ProgressPhase::allocating && progress.completed == 1U) {
            observed = true;
            source_.cancel();
        }
    }

    bool observed{};

  private:
    axk::CancellationSource &source_;
};

Json repeated_rows(std::size_t count) {
    auto rows = Json::array();
    for (std::size_t index = 0; index < count; ++index)
        rows.push_back({{"sample", "New"}});
    return rows;
}

class ProgramAssignmentReplacement : public testing::Test {
  protected:
    std::filesystem::path root;
    std::filesystem::path source;
    std::filesystem::path output;
    axk::ObjectCatalog before;
    std::string expected_hash;

    static axk::Result<axk::ObjectCatalog> catalog(const std::filesystem::path &path) {
        const auto image = axk::open_image(path);
        if (!image)
            return std::unexpected(image.error());
        return axk::build_object_catalog(*image);
    }

    void SetUp() override {
        const auto prefix = std::string{"axklib-assignment-replacement-"} +
                            testing::UnitTest::GetInstance()->current_test_info()->name();
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
        const auto audio = root / "tone.wav";
        axk::Waveform wave;
        wave.format = {1U, 2U, 44'100U};
        wave.frame_count = 4U;
        wave.pcm = {std::byte{0},    std::byte{0},    std::byte{0xe8}, std::byte{3},
                    std::byte{0x18}, std::byte{0xfc}, std::byte{0},    std::byte{0}};
        ASSERT_TRUE(axk::write_wav_atomic(audio, wave));
        axk::VolumeSpec volume;
        volume.name = "Programs";
        volume.waveforms.push_back({"wave", "Wave", audio, 60U, {}});
        std::vector<std::string> names{"Member", "Old", "New", "Other"};
        for (std::size_t index = 0; index < 40U; ++index)
            names.push_back(std::format("Extra{:02}", index));
        for (const auto &name : names) {
            axk::SampleSpec sample;
            sample.name = name;
            sample.waveform_id = "wave";
            volume.samples.push_back(sample);
        }
        volume.sample_banks.push_back({"Bank", {"Member"}});
        axk::ProgramSpec first{1U,
                               "First",
                               {{"SBAC", "Bank", {.receive = axk::ProgramReceiveInherit{}}},
                                {"SBNK", "Old", {.pan_offset = -20}},
                                {"SBNK", "Other", {.level_offset = 11}}}};
        first.parameters.level = 83U;
        first.parameters.transpose = -7;
        first.parameters.lfo.sample_hold_speed = 50U;
        volume.programs.push_back(first);
        volume.programs.push_back({2U, "Second", {{"SBNK", "Old", {.receive = axk::ProgramReceiveInherit{}}}}});
        const axk::HdsBuildManifest build{"1.0", 4U * 1024U * 1024U, {{"Partition", {volume}}}};
        const auto written = axk::write_hds_image(build, source);
        ASSERT_TRUE(written) << written.error().message;
        const auto initial = catalog(source);
        ASSERT_TRUE(initial);
        const auto *program = find_object(*initial, axk::ObjectType::prog, "001");
        ASSERT_NE(program, nullptr);
        const auto image = read_image(source);
        const auto found =
            std::search(image.begin(), image.end(), program->raw_payload.begin(), program->raw_payload.end(),
                        [](char left, std::byte right) {
                            return static_cast<unsigned char>(left) == std::to_integer<unsigned char>(right);
                        });
        ASSERT_NE(found, image.end());
        {
            std::fstream file{source, std::ios::binary | std::ios::in | std::ios::out};
            ASSERT_TRUE(file);
            for (const auto offset : {0x43U, 0x120U + 0x36U, 0x120U + 0x38U + 0x10U, 0x120U + 7U * 0x38U + 0x37U}) {
                file.seekp(static_cast<std::streamoff>(found - image.begin()) + static_cast<std::streamoff>(offset));
                file.put('\x5a');
                ASSERT_TRUE(file);
            }
        }
        const auto patched = catalog(source);
        ASSERT_TRUE(patched);
        before = *patched;
        const auto *saved = find_object(before, axk::ObjectType::prog, "001");
        ASSERT_NE(saved, nullptr);
        expected_hash = axk::package_internal::hex_digest(axk::package_internal::sha256(saved->raw_payload));
    }

    void TearDown() override {
        std::error_code error;
        if (!root.empty())
            std::filesystem::remove_all(root, error);
    }

    void expect_tail_preserved(const axk::ObjectSnapshot &old, const axk::ObjectSnapshot &current) {
        const auto &old_program = std::get<axk::CurrentProg>(old.object.payload);
        const auto &program = std::get<axk::CurrentProg>(current.object.payload);
        ASSERT_TRUE(old_program.layout.parameter_tail_offset);
        ASSERT_TRUE(program.layout.parameter_tail_offset);
        EXPECT_TRUE(std::ranges::equal(std::span{old.raw_payload}.subspan(*old_program.layout.parameter_tail_offset),
                                       std::span{current.raw_payload}.subspan(*program.layout.parameter_tail_offset)));
        EXPECT_EQ(program.parameters.level, old_program.parameters.level);
        EXPECT_EQ(program.parameters.transpose, old_program.parameters.transpose);
        EXPECT_EQ(current.raw_payload[0x43U], std::byte{0x5a});
    }

    void expect_sample_links(const axk::ObjectCatalog &objects, std::string_view name,
                             const std::vector<std::uint8_t> &programs) {
        const auto *object = find_object(objects, axk::ObjectType::sbnk, name);
        ASSERT_NE(object, nullptr);
        EXPECT_EQ(std::get<axk::CurrentSbnk>(object->object.payload).linked_program_numbers, programs);
    }
};

TEST(ProgramAssignmentReplacementManifest, EnforcesHashModelAndUnambiguousRowShapesAndCount) {
    const std::string hash(64U, 'a');
    EXPECT_TRUE(parse_replacement(replacement(hash, Json::array())));
    EXPECT_TRUE(parse_replacement(replacement(hash, Json::array({{{"retain_ordinal", 0}}}))));
    for (const auto &bad_hash :
         {std::string{}, std::string(63U, 'a'), std::string(65U, 'a'), std::string(64U, 'A'), std::string(64U, 'g')})
        EXPECT_FALSE(parse_replacement(replacement(bad_hash, Json::array())));
    auto no_model = replacement(hash, Json::array());
    no_model.erase("model");
    EXPECT_FALSE(parse_replacement(no_model));
    const std::vector<Json> invalid_rows{Json::array({Json::object()}),
                                         Json::array({{{"retain_ordinal", -1}}}),
                                         Json::array({{{"retain_ordinal", 0}}, {{"retain_ordinal", 0}}}),
                                         Json::array({{{"sample", "Old"}, {"sample_bank", "Bank"}}}),
                                         Json::array({{{"sample", ""}}}),
                                         Json::array({{{"retain_ordinal", 0}, {"unknown", true}}})};
    for (const auto &rows : invalid_rows) {
        SCOPED_TRACE(rows.dump());
        EXPECT_FALSE(parse_replacement(replacement(hash, rows)));
    }
    auto too_many = Json::array();
    for (std::size_t index = 0; index < 1000U; ++index)
        too_many.push_back({{"sample", std::format("S{}", index)}});
    EXPECT_FALSE(parse_replacement(replacement(hash, too_many)));
}

TEST_F(ProgramAssignmentReplacement, ReordersRetargetsAndAppendsWhilePreservingRetainedRowsTailAndSpareCapacity) {
    const auto parsed = parse_replacement(
        replacement(expected_hash, Json::array({{{"retain_ordinal", 2}},
                                                {{"retain_ordinal", 1}, {"sample", "New"}},
                                                {{"retain_ordinal", 0}},
                                                {{"sample", "Extra00"}, {"parameters", {{"pan_offset", 7}}}}})));
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto original = read_image(source);
    const auto changed = axk::alter_hds(source, *parsed, output);
    ASSERT_TRUE(changed) << changed.error().message;
    EXPECT_EQ(read_image(source), original);
    const auto after = catalog(output);
    ASSERT_TRUE(after);
    const auto *old = find_object(before, axk::ObjectType::prog, "001");
    const auto *current = find_object(*after, axk::ObjectType::prog, "001");
    ASSERT_NE(old, nullptr);
    ASSERT_NE(current, nullptr);
    const auto &previous = std::get<axk::CurrentProg>(old->object.payload);
    const auto &program = std::get<axk::CurrentProg>(current->object.payload);
    ASSERT_EQ(program.assignments.size(), 4U);
    EXPECT_EQ(program.layout.assignment_capacity, 8U);
    EXPECT_EQ(program.assignments[0].raw_row, previous.assignments[2].raw_row);
    EXPECT_EQ(program.assignments[2].raw_row, previous.assignments[0].raw_row);
    EXPECT_EQ(program.assignments[1].name, "New");
    EXPECT_EQ(program.assignments[1].raw_handle, 0U);
    EXPECT_EQ(program.assignments[1].parameters.pan_offset, -20);
    EXPECT_TRUE(std::ranges::equal(std::span{program.assignments[1].raw_row}.subspan(0x14U),
                                   std::span{previous.assignments[1].raw_row}.subspan(0x14U)));
    axk::ProgramSpec neutral{1U, "Fresh", {{"SBNK", "Extra00", {.pan_offset = 7}}}};
    const auto fresh = axk::detail::prepare_prog_payload(neutral);
    ASSERT_TRUE(fresh);
    EXPECT_TRUE(std::ranges::equal(program.assignments[3].raw_row, std::span{*fresh}.subspan(0x120U, 0x38U)));
    EXPECT_TRUE(std::ranges::equal(std::span{current->raw_payload}.subspan(0x120U + 4U * 0x38U, 4U * 0x38U),
                                   std::span{old->raw_payload}.subspan(0x120U + 4U * 0x38U, 4U * 0x38U)));
    expect_tail_preserved(*old, *current);
    expect_sample_links(*after, "Old", {2U});
    expect_sample_links(*after, "New", {1U});
    expect_sample_links(*after, "Other", {1U});
    expect_sample_links(*after, "Extra00", {1U});
    const auto *second_before = find_object(before, axk::ObjectType::prog, "002");
    const auto *second_after = find_object(*after, axk::ObjectType::prog, "002");
    ASSERT_NE(second_before, nullptr);
    ASSERT_NE(second_after, nullptr);
    EXPECT_EQ(second_after->raw_payload, second_before->raw_payload);
}

TEST_F(ProgramAssignmentReplacement, ShrinksToZeroNeutralizingRemovedRowsWithoutShrinkingCapacityOrOtherProgramLinks) {
    const auto parsed = parse_replacement(replacement(expected_hash, Json::array()));
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto changed = axk::alter_hds(source, *parsed, output);
    ASSERT_TRUE(changed) << changed.error().message;
    const auto after = catalog(output);
    ASSERT_TRUE(after);
    const auto *old = find_object(before, axk::ObjectType::prog, "001");
    const auto *current = find_object(*after, axk::ObjectType::prog, "001");
    ASSERT_NE(old, nullptr);
    ASSERT_NE(current, nullptr);
    const auto &program = std::get<axk::CurrentProg>(current->object.payload);
    EXPECT_TRUE(program.assignments.empty());
    EXPECT_EQ(program.layout.assignment_capacity, 8U);
    const auto neutral = axk::detail::prepare_prog_payload({1U, "Empty", {}});
    ASSERT_TRUE(neutral);
    EXPECT_TRUE(std::ranges::equal(std::span{current->raw_payload}.subspan(0x120U, 3U * 0x38U),
                                   std::span{*neutral}.subspan(0x120U, 3U * 0x38U)));
    EXPECT_TRUE(std::ranges::equal(std::span{current->raw_payload}.subspan(0x120U + 3U * 0x38U, 5U * 0x38U),
                                   std::span{old->raw_payload}.subspan(0x120U + 3U * 0x38U, 5U * 0x38U)));
    expect_tail_preserved(*old, *current);
    expect_sample_links(*after, "Old", {2U});
    expect_sample_links(*after, "Other", {});
    const auto *bank = find_object(*after, axk::ObjectType::sbac, "Bank");
    ASSERT_NE(bank, nullptr);
    EXPECT_EQ(std::get<axk::CurrentSbac>(bank->object.payload).raw_sample_parameter_block[0x1bU], std::byte{0});
}

TEST_F(ProgramAssignmentReplacement, GrowsAcrossAllocationBoundaryPreservingTailAndObjectIdentity) {
    auto rows = Json::array();
    for (std::size_t index = 0; index < 40U; ++index)
        rows.push_back({{"sample", std::format("Extra{:02}", index)}});
    const auto parsed = parse_replacement(replacement(expected_hash, rows));
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto changed = axk::alter_hds(source, *parsed, output);
    ASSERT_TRUE(changed) << changed.error().message;
    const auto after = catalog(output);
    ASSERT_TRUE(after);
    const auto *old = find_object(before, axk::ObjectType::prog, "001");
    const auto *current = find_object(*after, axk::ObjectType::prog, "001");
    ASSERT_NE(old, nullptr);
    ASSERT_NE(current, nullptr);
    const auto &program = std::get<axk::CurrentProg>(current->object.payload);
    EXPECT_EQ(current->key, old->key);
    EXPECT_GT(current->raw_payload.size(), 2048U);
    EXPECT_EQ(program.layout.assignment_capacity, 40U);
    ASSERT_EQ(program.assignments.size(), 40U);
    for (std::size_t index = 0; index < 40U; ++index) {
        EXPECT_EQ(program.assignments[index].name, std::format("Extra{:02}", index));
        expect_sample_links(*after, std::format("Extra{:02}", index), {1U});
    }
    expect_tail_preserved(*old, *current);
    expect_sample_links(*after, "Old", {2U});
}

TEST_F(ProgramAssignmentReplacement, StaleGuardInvalidRowsAndLaterFailurePreserveSourceAndExistingDestination) {
    const auto original = read_image(source);
    std::filesystem::copy_file(source, output);
    const auto stale = parse_replacement(replacement(std::string(64U, '0'), Json::array()));
    ASSERT_TRUE(stale);
    EXPECT_FALSE(axk::alter_hds(source, *stale, output, {}, nullptr, true));
    EXPECT_EQ(read_image(output), original);
    for (const auto &rows : {Json::array({{{"retain_ordinal", 3}}}), Json::array({{{"sample", "Absent"}}})}) {
        const auto invalid = parse_replacement(replacement(expected_hash, rows));
        ASSERT_TRUE(invalid);
        EXPECT_FALSE(axk::alter_hds(source, *invalid, output, {}, nullptr, true));
        EXPECT_EQ(read_image(source), original);
        EXPECT_EQ(read_image(output), original);
    }
    auto later = parse_replacement(replacement(expected_hash, Json::array()));
    ASSERT_TRUE(later);
    later->operations.push_back({"missing", axk::DeleteProgramOperation{axk::PartitionIndex{0}, "Programs", 128U}});
    EXPECT_FALSE(axk::alter_hds(source, *later, output, {}, nullptr, true));
    EXPECT_EQ(read_image(source), original);
    EXPECT_EQ(read_image(output), original);
}

TEST_F(ProgramAssignmentReplacement, GrowsTo999CountedAssignmentsAndRetainsOneTargetMembershipBit) {
    const auto parsed = parse_replacement(replacement(expected_hash, repeated_rows(999U)));
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto original = read_image(source);

    const auto changed = axk::alter_hds(source, *parsed, output);

    ASSERT_TRUE(changed) << changed.error().message;
    EXPECT_EQ(read_image(source), original);
    const auto after = catalog(output);
    ASSERT_TRUE(after) << after.error().message;
    const auto *old = find_object(before, axk::ObjectType::prog, "001");
    const auto *current = find_object(*after, axk::ObjectType::prog, "001");
    ASSERT_NE(old, nullptr);
    ASSERT_NE(current, nullptr);
    const auto &program = std::get<axk::CurrentProg>(current->object.payload);
    ASSERT_EQ(program.assignments.size(), 999U);
    EXPECT_EQ(program.layout.stored_assignment_count, 999U);
    EXPECT_EQ(program.layout.assignment_capacity, 999U);
    EXPECT_EQ(current->key, old->key);
    for (const auto &row : program.assignments) {
        EXPECT_EQ(row.name, "New");
        EXPECT_EQ(row.kind, 0x10U);
        EXPECT_EQ(row.raw_handle, 0U);
    }
    expect_tail_preserved(*old, *current);
    expect_sample_links(*after, "New", {1U});
    expect_sample_links(*after, "Old", {2U});
    expect_sample_links(*after, "Other", {});
    const auto opened = axk::open_image(output);
    ASSERT_TRUE(opened);
    EXPECT_TRUE(axk::allocation_is_safe_for_mutation(opened->partitions().front().allocation));
}

TEST_F(ProgramAssignmentReplacement, CancellationAfterQueuedGrowthPreservesSourceAndExistingDestination) {
    const auto parsed = parse_replacement(replacement(expected_hash, repeated_rows(999U)));
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto original = read_image(source);
    std::filesystem::copy_file(source, output);
    axk::CancellationSource cancellation;
    CancelAfterReplacement progress{cancellation};

    const auto changed = axk::alter_hds(source, *parsed, output, cancellation.token(), &progress, true);

    EXPECT_TRUE(progress.observed);
    ASSERT_FALSE(changed);
    EXPECT_EQ(changed.error().code, axk::ErrorCode::operation_cancelled);
    EXPECT_EQ(read_image(source), original);
    EXPECT_EQ(read_image(output), original);
}

TEST_F(ProgramAssignmentReplacement, ExhaustedImageAllocationRollsBackGrowthAndAllTargetBitmapChanges) {
    std::uint32_t free_clusters{};
    {
        const auto image = axk::open_image(source);
        ASSERT_TRUE(image);
        const auto &allocation = image->partitions().front().allocation;
        ASSERT_TRUE(axk::allocation_is_safe_for_mutation(allocation));
        ASSERT_TRUE(allocation.free_space);
        free_clusters = allocation.free_space->free_cluster_count;
    }
    ASSERT_GT(free_clusters, 64U);
    // Leave eight 1 KiB clusters free, below the additional capacity needed by 999 rows.
    const auto pcm_bytes = static_cast<std::size_t>(free_clusters - 8U) * 1024U - 512U - 8U;
    axk::Waveform fill;
    fill.format = {1U, 2U, 44'100U};
    fill.frame_count = pcm_bytes / 2U;
    fill.pcm.resize(pcm_bytes);
    const auto audio = root / "fill.wav";
    ASSERT_TRUE(axk::write_wav_atomic(audio, fill));
    axk::InsertWaveformSpec waveform;
    waveform.path = audio;
    waveform.waveform_names = {"Capacity Fill"};
    waveform.root_key = 60U;
    waveform.loop_mode = axk::AudioSamplerLoopMode::forward;
    axk::AlterationManifest filling;
    filling.schema_version = "1.0";
    filling.operations.push_back({"fill", axk::InsertWaveformOperation{axk::PartitionIndex{0}, "Programs", waveform}});
    const auto filled = root / "filled.hds";
    const auto inserted = axk::alter_hds(source, filling, filled);
    ASSERT_TRUE(inserted) << inserted.error().message;
    {
        const auto image = axk::open_image(filled);
        ASSERT_TRUE(image);
        const auto &allocation = image->partitions().front().allocation;
        ASSERT_TRUE(axk::allocation_is_safe_for_mutation(allocation));
        ASSERT_TRUE(allocation.free_space);
        ASSERT_LE(allocation.free_space->free_cluster_count, 8U);
    }
    const auto parsed = parse_replacement(replacement(expected_hash, repeated_rows(999U)));
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto original = read_image(filled);
    std::filesystem::copy_file(filled, output);

    const auto changed = axk::alter_hds(filled, *parsed, output, {}, nullptr, true);

    ASSERT_FALSE(changed);
    EXPECT_EQ(changed.error().message, "partition has insufficient free clusters");
    EXPECT_EQ(read_image(filled), original);
    EXPECT_EQ(read_image(output), original);
}

} // namespace
