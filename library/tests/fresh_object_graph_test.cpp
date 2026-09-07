#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/media.hpp"
#include "axklib/relationship.hpp"
#include "axklib/writer.hpp"

namespace {

class FreshObjectGraph : public testing::Test {
  protected:
    std::filesystem::path root;
    axk::VolumeSpec volume;

    void SetUp() override {
        const auto prefix =
            std::string{"axklib-fresh-graph-"} + testing::UnitTest::GetInstance()->current_test_info()->name();
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
        axk::Waveform waveform;
        waveform.format = {1U, 2U, 44'100U};
        waveform.frame_count = 4U;
        waveform.pcm = {std::byte{0},    std::byte{0},    std::byte{0xe8}, std::byte{3},
                        std::byte{0x18}, std::byte{0xfc}, std::byte{0},    std::byte{0}};
        const auto audio = root / "tone.wav";
        ASSERT_TRUE(axk::write_wav_atomic(audio, waveform));
        volume.name = "Graph";
        volume.waveforms.push_back({"left", "Left Wave", audio, 60U, {}});
        volume.waveforms.push_back({"right", "Right Wave", audio, 60U, {}});
    }

    void TearDown() override {
        std::error_code error;
        if (!root.empty())
            std::filesystem::remove_all(root, error);
    }

    void add_sample(std::string name, bool stereo = false) {
        axk::SampleSpec sample;
        sample.name = std::move(name);
        sample.waveform_id = "left";
        if (stereo)
            sample.right_waveform_id = "right";
        volume.samples.push_back(std::move(sample));
    }

    axk::Result<axk::ObjectCatalog> write_catalog() {
        const auto output = root / "graph.hds";
        const axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {{"Partition", {volume}}}};
        const auto written = axk::write_hds_image(manifest, output);
        if (!written)
            return std::unexpected(written.error());
        const auto opened = axk::open_media(output);
        if (!opened)
            return std::unexpected(opened.error());
        return axk::build_object_catalog(*opened);
    }

    template <class Payload> static const Payload *find(const axk::ObjectCatalog &catalog, std::string_view name) {
        for (const auto &entry : catalog.objects) {
            if (entry.object.header.name == name) {
                if (const auto *payload = std::get_if<Payload>(&entry.object.payload))
                    return payload;
            }
        }
        return nullptr;
    }
};

TEST_F(FreshObjectGraph, WritesStandaloneSampleBankWithoutPrograms) {
    add_sample("Member");
    volume.sample_banks.push_back({"Bank", {"Member"}});

    const auto catalog = write_catalog();

    ASSERT_TRUE(catalog) << catalog.error().message;
    const auto *bank = find<axk::CurrentSbac>(*catalog, "Bank");
    ASSERT_NE(bank, nullptr);
    ASSERT_EQ(bank->effective_member_count, 1U);
    const auto *sample = find<axk::CurrentSbnk>(*catalog, "Member");
    ASSERT_NE(sample, nullptr);
    EXPECT_TRUE(sample->linked_program_numbers.empty());
    EXPECT_EQ(sample->sample_flags & 1U, 1U);
}

TEST_F(FreshObjectGraph, WritesDirectStereoSampleWithInheritedReceive) {
    add_sample("Stereo", true);
    volume.programs.push_back({1U, "Stereo", {{"SBNK", "Stereo", {.receive = axk::ProgramReceiveInherit{}}}}});

    const auto catalog = write_catalog();

    ASSERT_TRUE(catalog) << catalog.error().message;
    const auto *sample = find<axk::CurrentSbnk>(*catalog, "Stereo");
    ASSERT_NE(sample, nullptr);
    EXPECT_TRUE(sample->right_slot_present);
    ASSERT_TRUE(sample->right);
    EXPECT_EQ(sample->left.wave_data_name, "Left Wave");
    EXPECT_EQ(sample->right->wave_data_name, "Right Wave");
    EXPECT_EQ(sample->linked_program_numbers, std::vector<std::uint8_t>{1U});
    const auto *program = find<axk::CurrentProg>(*catalog, "001");
    ASSERT_NE(program, nullptr);
    ASSERT_EQ(program->assignments.size(), 1U);
    EXPECT_EQ(program->assignments.front().raw_receive_selector, 0xffU);
}

TEST_F(FreshObjectGraph, WritesMultipleBanksAndDirectSamplesWithIndependentReceiveSettings) {
    for (const auto *name : {"Member A", "Member B", "Direct A", "Direct B"})
        add_sample(name);
    volume.sample_banks = {{"Bank A", {"Member A"}}, {"Bank B", {"Member B"}}};
    volume.programs.push_back({1U,
                               "Mixed",
                               {{"SBNK", "Direct A", {.receive = axk::ProgramReceiveBasic{}}},
                                {"SBAC", "Bank B", {.receive = axk::ProgramReceiveChannel{axk::MidiPort::a, 16U}}},
                                {"SBAC", "Bank A", {.receive = axk::ProgramReceiveInherit{}}},
                                {"SBNK", "Direct B", {.receive = axk::ProgramReceiveChannel{axk::MidiPort::a, 4U}}}}});

    const auto catalog = write_catalog();

    ASSERT_TRUE(catalog) << catalog.error().message;
    const auto *program = find<axk::CurrentProg>(*catalog, "001");
    ASSERT_NE(program, nullptr);
    ASSERT_EQ(program->assignments.size(), 4U);
    EXPECT_EQ(program->assignments[0].name, "Direct A");
    EXPECT_EQ(program->assignments[1].name, "Bank B");
    EXPECT_EQ(program->assignments[2].name, "Bank A");
    EXPECT_EQ(program->assignments[3].name, "Direct B");
    EXPECT_EQ(program->assignments[0].raw_receive_selector, 16U);
    EXPECT_EQ(program->assignments[1].raw_receive_selector, 15U);
    EXPECT_EQ(program->assignments[2].raw_receive_selector, 0xffU);
    EXPECT_EQ(program->assignments[3].raw_receive_selector, 3U);
}

TEST_F(FreshObjectGraph, DerivesMembershipBitmapsFromTargetsSharedAcrossPrograms) {
    add_sample("Member");
    add_sample("Direct");
    volume.sample_banks.push_back({"Bank", {"Member"}});
    for (const auto number : std::vector<std::uint8_t>{1U, 33U, 128U})
        volume.programs.push_back(
            {number,
             "Shared",
             {{"SBAC", "Bank", {.receive = axk::ProgramReceiveChannel{axk::MidiPort::a, 1U}}},
              {"SBNK", "Direct", {.receive = axk::ProgramReceiveChannel{axk::MidiPort::a, 2U}}}}});

    const auto catalog = write_catalog();

    ASSERT_TRUE(catalog) << catalog.error().message;
    const auto *direct = find<axk::CurrentSbnk>(*catalog, "Direct");
    ASSERT_NE(direct, nullptr);
    EXPECT_EQ(direct->linked_program_numbers, (std::vector<std::uint8_t>{1U, 33U, 128U}));
    const auto *member = find<axk::CurrentSbnk>(*catalog, "Member");
    ASSERT_NE(member, nullptr);
    EXPECT_TRUE(member->linked_program_numbers.empty());
    const auto *bank = find<axk::CurrentSbac>(*catalog, "Bank");
    ASSERT_NE(bank, nullptr);
    EXPECT_EQ(bank->raw_sample_parameter_block[0x1bU], std::byte{1});
    EXPECT_EQ(bank->raw_sample_parameter_block[0x1fU], std::byte{1});
    EXPECT_EQ(bank->raw_sample_parameter_block[0x24U], std::byte{0x80});
    const auto graph = axk::build_relationship_graph(*catalog);
    EXPECT_EQ(std::ranges::count_if(graph.relationships,
                                    [](const auto &row) {
                                        return axk::is_effective_program_assignment(row) && row.target_key.has_value();
                                    }),
              6);
    for (const auto &comparison : graph.bitmap_comparisons)
        EXPECT_TRUE(comparison.direct_without_bitmap.empty());
}

TEST_F(FreshObjectGraph, KeepsBankMemberAndDirectSampleSourceAndParametersIndependent) {
    add_sample("Member");
    add_sample("Direct");
    volume.samples[0].parameters.level = 75U;
    volume.samples[0].parameters.key_low = 12U;
    volume.samples[0].parameters.key_high = 60U;
    volume.samples[1].waveform_id = "right";
    volume.samples[1].parameters.level = 93U;
    volume.samples[1].parameters.key_low = 61U;
    volume.samples[1].parameters.key_high = 100U;
    volume.sample_banks.push_back({"Bank", {"Member"}});
    volume.programs.push_back({1U,
                               "Separate",
                               {{"SBAC", "Bank", {.receive = axk::ProgramReceiveChannel{axk::MidiPort::a, 1U}}},
                                {"SBNK", "Direct", {.receive = axk::ProgramReceiveChannel{axk::MidiPort::a, 2U}}}}});

    const auto catalog = write_catalog();

    ASSERT_TRUE(catalog) << catalog.error().message;
    const auto *member = find<axk::CurrentSbnk>(*catalog, "Member");
    const auto *direct = find<axk::CurrentSbnk>(*catalog, "Direct");
    ASSERT_NE(member, nullptr);
    ASSERT_NE(direct, nullptr);
    EXPECT_EQ(member->left.wave_data_name, "Left Wave");
    EXPECT_EQ(direct->left.wave_data_name, "Right Wave");
    EXPECT_EQ(member->sample_level, 75U);
    EXPECT_EQ(direct->sample_level, 93U);
    EXPECT_EQ(member->key_range_low, 12U);
    EXPECT_EQ(member->key_range_high, 60U);
    EXPECT_EQ(direct->key_range_low, 61U);
    EXPECT_EQ(direct->key_range_high, 100U);
}

TEST_F(FreshObjectGraph, RejectsMissingProgramTargetWithoutPublishingImage) {
    add_sample("Sample");
    volume.programs.push_back({1U, "Missing", {{"SBNK", "Absent", {}}}});

    EXPECT_FALSE(write_catalog());
    EXPECT_FALSE(std::filesystem::exists(root / "graph.hds"));
}

TEST_F(FreshObjectGraph, RejectsSampleMembershipInTwoBanksWithoutPublishingImage) {
    add_sample("Member");
    volume.sample_banks = {{"Bank A", {"Member"}}, {"Bank B", {"Member"}}};

    EXPECT_FALSE(write_catalog());
    EXPECT_FALSE(std::filesystem::exists(root / "graph.hds"));
}

} // namespace
