#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/application/file_operations.hpp"
#include "axklib/application/volume_graph.hpp"
#include "axklib/audio.hpp"
#include "axklib/audio_export.hpp"
#include "axklib/catalog.hpp"
#include "axklib/relationship.hpp"
#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

#ifdef AXK_EXPORT_MEMORY_TEST
#include <sys/resource.h>
#endif

namespace {

class ExportParameterOwnership : public testing::Test {
  protected:
    std::filesystem::path root;

    void SetUp() override {
        const auto prefix =
            std::string{"axklib-export-owner-"} + testing::UnitTest::GetInstance()->current_test_info()->name();
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
    }

    void TearDown() override {
        std::error_code error;
        if (!root.empty())
            std::filesystem::remove_all(root, error);
    }

    void verify_shared_wave_owners(std::size_t count, bool stereo) {
        axk::Waveform waveform;
        waveform.format = {1U, 2U, 44'100U};
        waveform.frame_count = 4U;
        waveform.pcm = {std::byte{0},    std::byte{0},    std::byte{0x20}, std::byte{0x4e},
                        std::byte{0xe0}, std::byte{0xb1}, std::byte{0},    std::byte{0}};
        const auto audio = root / "shared.wav";
        ASSERT_TRUE(axk::write_wav_atomic(audio, waveform));
        axk::VolumeSpec spec;
        spec.name = "Owners";
        spec.waveforms.push_back({"left", "Left", audio, 60U, {}});
        if (stereo)
            spec.waveforms.push_back({"right", "Right", audio, 60U, {}});
        for (std::size_t index = 0; index < count; ++index) {
            axk::SampleSpec sample;
            sample.name = "Owner " + std::to_string(index);
            sample.waveform_id = "left";
            if (stereo)
                sample.right_waveform_id = "right";
            sample.parameters.key_low = static_cast<std::uint8_t>(index % 64U);
            sample.parameters.key_high = static_cast<std::uint8_t>(64U + index % 64U);
            sample.parameters.root_key = static_cast<std::uint8_t>(36U + index % 48U);
            spec.samples.push_back(std::move(sample));
        }
        const auto path = root / "owners.hds";
        const axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {{"Partition", {spec}}}};
        const auto written = axk::write_hds_image(manifest, path);
        ASSERT_TRUE(written) << written.error().message;
        const auto container = axk::open_image(path);
        ASSERT_TRUE(container) << container.error().message;
        const auto catalog = axk::build_object_catalog(*container);
        ASSERT_TRUE(catalog) << catalog.error().message;
        const auto graph = axk::build_relationship_graph(*catalog);
        const auto plan = axk::build_export_plan(*container, *catalog, graph);
        ASSERT_TRUE(plan) << plan.error().message;
        ASSERT_EQ(plan->volumes.size(), 1U);
        const auto &volume = plan->volumes.front();
        ASSERT_EQ(volume.samples.size(), count);
        ASSERT_EQ(volume.waveforms.size(), stereo ? 2U : 1U);
        for (const auto &physical : volume.waveforms) {
            EXPECT_EQ(physical.user_facing_aliases.size(), count);
            ASSERT_GE(physical.waveform.pcm.size(), waveform.pcm.size());
            EXPECT_TRUE(std::equal(waveform.pcm.begin(), waveform.pcm.end(), physical.waveform.pcm.begin()));
        }
        const auto serialized = axk::app::serialize_volume_graph(volume, graph, path);
        ASSERT_TRUE(serialized) << serialized.error().message;
        const auto document = nlohmann::json::parse(*serialized);
        const auto &samples = document.at("objects").at("sbnk");
        ASSERT_EQ(samples.size(), count);
        std::set<std::pair<std::string, std::string>> exported_edges;
        for (const auto &relationship : document.at("relationships")) {
            exported_edges.emplace(relationship.at("source_key").get<std::string>(),
                                   relationship.at("target_key").get<std::string>());
        }
        EXPECT_EQ(exported_edges.size(), count * (stereo ? 2U : 1U));
        std::size_t context_count = 0U;
        for (std::size_t index = 0; index < count; ++index) {
            const auto &sample = samples.at(index);
            const auto &owner = volume.samples.at(index);
            const auto &parameters = sample.at("parameters");
            const auto &contexts = parameters.at("decoded_current_sbnk_member_parameters");
            context_count += contexts.size();
            ASSERT_EQ(contexts.size(), 1U) << owner.display_name;
            const auto &context = contexts.front();
            EXPECT_EQ(context.at("sample_object_key"), owner.object_key);
            EXPECT_EQ(context.at("sample_name"), owner.display_name);
            EXPECT_EQ(context.at("left_member").at("root_key"), owner.decoded.left.root_key);
            const auto source_index = std::stoul(owner.display_name.substr(6U));
            EXPECT_EQ(owner.decoded.key_range_low, source_index % 64U);
            EXPECT_EQ(owner.decoded.key_range_high, 64U + source_index % 64U);
            EXPECT_EQ(owner.decoded.left.root_key, 36U + source_index % 48U);
            EXPECT_EQ(parameters.at("resolved_key_range").at("low_midi"), owner.decoded.key_range_low);
            EXPECT_EQ(parameters.at("resolved_key_range").at("high_midi"), owner.decoded.key_range_high);
            const auto &members = sample.at("physical_waveforms");
            ASSERT_EQ(members.size(), stereo ? 2U : 1U);
            for (const auto &member : members) {
                EXPECT_TRUE(exported_edges.contains({owner.object_key, member.at("smpl_id").get<std::string>()}));
            }
            if (stereo) {
                EXPECT_EQ(members.at(0).at("role"), "left");
                EXPECT_EQ(members.at(1).at("role"), "right");
                EXPECT_NE(members.at(0).at("smpl_id"), members.at(1).at("smpl_id"));
                EXPECT_EQ(context.at("right_member").at("root_key"), owner.decoded.right->root_key);
            } else {
                EXPECT_EQ(members.at(0).at("role"), "left");
                EXPECT_TRUE(context.at("right_member").is_null());
            }
        }
        EXPECT_EQ(context_count, count);
    }
};

#ifdef AXK_EXPORT_MEMORY_TEST
TEST_F(ExportParameterOwnership, CorpusAuditReleasesEachCompletedSource) {
    axk::Waveform waveform;
    waveform.format = {1U, 2U, 44'100U};
    waveform.frame_count = 8U * 1024U * 1024U;
    waveform.pcm.resize(static_cast<std::size_t>(waveform.frame_count) * 2U);
    const auto audio = root / "large.wav";
    ASSERT_TRUE(axk::write_wav_atomic(audio, waveform));
    axk::VolumeSpec volume;
    volume.name = "Audit";
    volume.waveforms.push_back({"wave", "Wave", audio, 60U, {}});
    const axk::HdsBuildManifest manifest{"1.0", 32U * 1024U * 1024U, {{"Partition", {volume}}}};
    ASSERT_TRUE(axk::write_hds_image(manifest, root / "source.hds"));
    auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root, true}});
    ASSERT_TRUE(sandbox);
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_file_operations(registry, *sandbox));
    auto sources = nlohmann::json::array();
    for (std::size_t index = 0; index < 64U; ++index)
        sources.push_back({{"rootId", "workspace"}, {"relativePath", "source.hds"}});
    const auto result = registry.invoke(
        "corpus.audit", {{"sources", sources}, {"destination", {{"rootId", "workspace"}, {"relativePath", "report"}}}},
        {.owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->at("loadedCount"), 64U);
    EXPECT_EQ(result->at("objectCount"), 64U);
    EXPECT_EQ(result->at("waveSmokeDecoded"), 64U);
    EXPECT_EQ(result->at("failedCount"), 0U);
    rusage usage{};
    ASSERT_EQ(getrusage(RUSAGE_SELF, &usage), 0);
    std::cout << "Corpus peak RSS: " << usage.ru_maxrss << " KiB\n";
    EXPECT_LE(usage.ru_maxrss, 512L * 1024L);
}

TEST_F(ExportParameterOwnership, ThousandSharedWaveOwnersStayWithinMemoryBudget) {
    verify_shared_wave_owners(999U, false);
    rusage usage{};
    ASSERT_EQ(getrusage(RUSAGE_SELF, &usage), 0);
    std::cout << "Peak RSS: " << usage.ru_maxrss << " KiB\n";
    EXPECT_LE(usage.ru_maxrss, 512L * 1024L);
}
#else
TEST_F(ExportParameterOwnership, OneOwnerHasOneParameterContext) { verify_shared_wave_owners(1U, false); }
TEST_F(ExportParameterOwnership, SeventeenOwnersHaveIndependentParameterContexts) {
    verify_shared_wave_owners(17U, false);
}
TEST_F(ExportParameterOwnership, BankSizedCollectionHasLinearParameterContexts) {
    verify_shared_wave_owners(127U, false);
}
TEST_F(ExportParameterOwnership, SharedStereoMembersPreserveOwnerParametersAndRoles) {
    verify_shared_wave_owners(17U, true);
}
#ifndef __linux__
TEST_F(ExportParameterOwnership, ThousandSharedWaveOwnersHaveLinearParameterContexts) {
    verify_shared_wave_owners(999U, false);
}
#endif
#endif

} // namespace

#ifdef AXK_EXPORT_MEMORY_TEST
int main(int argc, char **argv) {
    const rlimit memory{2ULL * 1024ULL * 1024ULL * 1024ULL, 2ULL * 1024ULL * 1024ULL * 1024ULL};
    const rlimit core{0U, 0U};
    if (setrlimit(RLIMIT_AS, &memory) != 0 || setrlimit(RLIMIT_CORE, &core) != 0) {
        std::cerr << "Could not install export-test resource limits\n";
        return 1;
    }
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
