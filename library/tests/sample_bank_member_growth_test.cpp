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

#include "axklib/alteration.hpp"
#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/writer.hpp"

namespace {

std::vector<char> read_image_bytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
}

const axk::ObjectSnapshot *find_bank(const axk::ObjectCatalog &catalog) {
    const auto found = std::ranges::find_if(catalog.objects, [](const auto &object) {
        return object.object.header.type == axk::ObjectType::sbac && object.object.header.name == "Bank";
    });
    return found == catalog.objects.end() ? nullptr : &*found;
}

class SampleBankMemberGrowth : public testing::Test {
  protected:
    std::filesystem::path root;
    std::filesystem::path source;
    std::filesystem::path output;
    std::vector<std::string> original_members{"Member02", "Member00", "Member01"};
    std::vector<std::string> appended_members;

    void SetUp() override {
        const auto prefix =
            std::string{"axklib-bank-growth-"} + testing::UnitTest::GetInstance()->current_test_info()->name();
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
        axk::Waveform waveform;
        waveform.format = {1U, 2U, 44'100U};
        waveform.frame_count = 4U;
        waveform.pcm = {std::byte{0},    std::byte{0},    std::byte{0xe8}, std::byte{3},
                        std::byte{0x18}, std::byte{0xfc}, std::byte{0},    std::byte{0}};
        ASSERT_TRUE(axk::write_wav_atomic(audio, waveform));
        axk::VolumeSpec volume;
        volume.name = "Members";
        volume.waveforms.push_back({"wave", "Wave", audio, 60U, {}});
        for (std::size_t index = 0; index < 127U; ++index) {
            axk::SampleSpec sample;
            sample.name = std::format("Member{:02}", index);
            sample.waveform_id = "wave";
            sample.parameters.level = static_cast<std::uint8_t>(index);
            volume.samples.push_back(sample);
        }
        axk::SampleBankSpec bank{"Bank", original_members};
        bank.parameter_overrides.emplace();
        bank.parameter_overrides->level = 83U;
        bank.parameter_overrides->controls[5].device = 126U;
        bank.parameter_overrides->controls[5].function = 36U;
        bank.parameter_overrides->controls[5].type = 3U;
        bank.parameter_overrides->controls[5].range = -20;
        bank.parameter_overrides->output1_level = 90U;
        volume.sample_banks.push_back(bank);
        volume.programs.push_back({1U, "Bank", {{"SBAC", "Bank", {.receive = axk::ProgramReceiveInherit{}}}}});
        const axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {{"Partition", {volume}}}};
        const auto written = axk::write_hds_image(manifest, source);
        ASSERT_TRUE(written) << written.error().message;
        for (std::size_t index = 127U; index-- > 3U;)
            appended_members.push_back(std::format("Member{:02}", index));
    }

    void TearDown() override {
        std::error_code error;
        if (!root.empty())
            std::filesystem::remove_all(root, error);
    }

    static axk::Result<axk::ObjectCatalog> catalog(const std::filesystem::path &path) {
        const auto image = axk::open_image(path);
        if (!image)
            return std::unexpected(image.error());
        return axk::build_object_catalog(*image);
    }

    axk::AlterationManifest assignment() const {
        return {"1.0",
                {{"grow",
                  axk::AssignSampleBankMembersOperation{axk::PartitionIndex{0}, "Members", "Bank", appended_members}}}};
    }
};

TEST_F(SampleBankMemberGrowth, GrowsTo127MembersPreservingOrderSplitParametersAndExistingObjectState) {
    const auto before = catalog(source);
    ASSERT_TRUE(before);
    const auto *old = find_bank(*before);
    ASSERT_NE(old, nullptr);
    ASSERT_LE(old->raw_payload.size(), 1024U);
    const auto original = read_image_bytes(source);

    const auto changed = axk::alter_hds(source, assignment(), output);

    ASSERT_TRUE(changed) << changed.error().message;
    EXPECT_EQ(read_image_bytes(source), original);
    const auto after = catalog(output);
    ASSERT_TRUE(after) << after.error().message;
    const auto *current = find_bank(*after);
    ASSERT_NE(current, nullptr);
    EXPECT_EQ(current->key, old->key);
    EXPECT_GT(current->raw_payload.size(), 2048U);
    const auto &old_bank = std::get<axk::CurrentSbac>(old->object.payload);
    const auto &bank = std::get<axk::CurrentSbac>(current->object.payload);
    EXPECT_EQ(bank.stored_member_count, 127U);
    EXPECT_EQ(bank.effective_member_count, 127U);
    EXPECT_EQ(bank.raw_sample_parameter_block, old_bank.raw_sample_parameter_block);
    EXPECT_EQ(bank.pending_parameter_propagation_words, old_bank.pending_parameter_propagation_words);
    ASSERT_TRUE(old_bank.parameter_tail_offset);
    ASSERT_TRUE(bank.parameter_tail_offset);
    EXPECT_TRUE(std::ranges::equal(std::span{old->raw_payload}.subspan(*old_bank.parameter_tail_offset),
                                   std::span{current->raw_payload}.subspan(*bank.parameter_tail_offset)));
    auto expected_names = original_members;
    expected_names.insert(expected_names.end(), appended_members.begin(), appended_members.end());
    ASSERT_EQ(bank.slots.size(), expected_names.size());
    for (std::size_t index = 0; index < expected_names.size(); ++index) {
        EXPECT_TRUE(bank.slots[index].active);
        EXPECT_EQ(bank.slots[index].name, expected_names[index]);
    }
    ASSERT_EQ(before->objects.size(), after->objects.size());
    for (const auto &object : before->objects) {
        if (object.object.header.type == axk::ObjectType::sbac)
            continue;
        const auto found = std::ranges::find(after->objects, object.key, &axk::ObjectSnapshot::key);
        ASSERT_NE(found, after->objects.end());
        auto expected = object.raw_payload;
        if (object.object.header.type == axk::ObjectType::sbnk)
            expected[0xd0U] |= std::byte{1};
        EXPECT_EQ(found->raw_payload, expected) << object.object.header.name;
    }
    const auto image = axk::open_image(output);
    ASSERT_TRUE(image);
    EXPECT_TRUE(axk::allocation_is_safe_for_mutation(image->partitions().front().allocation));
}

TEST_F(SampleBankMemberGrowth, LaterOperationFailureRollsBackAllocationAndMembershipFlags) {
    auto operations = assignment();
    operations.operations.push_back({"missing", axk::DeleteProgramOperation{axk::PartitionIndex{0}, "Members", 128U}});
    const auto original = read_image_bytes(source);
    std::filesystem::copy_file(source, output);

    const auto changed = axk::alter_hds(source, operations, output, {}, nullptr, true);

    EXPECT_FALSE(changed);
    EXPECT_EQ(read_image_bytes(source), original);
    EXPECT_EQ(read_image_bytes(output), original);
}

TEST_F(SampleBankMemberGrowth, InsufficientFreeClustersPreservesSourceAndDestination) {
    std::uint32_t free_clusters{};
    {
        const auto image = axk::open_image(source);
        ASSERT_TRUE(image);
        ASSERT_TRUE(image->partitions().front().allocation.free_space);
        free_clusters = image->partitions().front().allocation.free_space->free_cluster_count;
    }
    ASSERT_GT(free_clusters, 10U);
    axk::Waveform waveform;
    waveform.format = {1U, 2U, 44'100U};
    // Exhaust allocation independently of the bank's existing extent capacity.
    const auto pcm_bytes = static_cast<std::size_t>(free_clusters) * 1024U - 512U - 8U;
    waveform.frame_count = pcm_bytes / 2U;
    waveform.pcm.resize(pcm_bytes);
    const auto audio = root / "fill.wav";
    ASSERT_TRUE(axk::write_wav_atomic(audio, waveform));
    axk::InsertWaveformSpec fill;
    fill.path = audio;
    fill.waveform_names = {"Fill"};
    fill.root_key = 60U;
    fill.loop_mode = axk::AudioSamplerLoopMode::forward;
    const axk::AlterationManifest filling{
        "1.0", {{"fill", axk::InsertWaveformOperation{axk::PartitionIndex{0}, "Members", fill}}}};
    const auto full_source = root / "full.hds";
    const auto filled = axk::alter_hds(source, filling, full_source);
    ASSERT_TRUE(filled) << filled.error().message;
    {
        const auto image = axk::open_image(full_source);
        ASSERT_TRUE(image);
        const auto &allocation = image->partitions().front().allocation;
        ASSERT_TRUE(axk::allocation_is_safe_for_mutation(allocation));
        ASSERT_TRUE(allocation.free_space);
        ASSERT_EQ(allocation.free_space->free_cluster_count, 0U);
    }
    const auto original = read_image_bytes(full_source);
    std::filesystem::copy_file(full_source, output);

    const auto changed = axk::alter_hds(full_source, assignment(), output, {}, nullptr, true);

    ASSERT_FALSE(changed);
    EXPECT_EQ(changed.error().message, "partition has insufficient free clusters");
    EXPECT_EQ(read_image_bytes(full_source), original);
    EXPECT_EQ(read_image_bytes(output), original);
}

} // namespace
