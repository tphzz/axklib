#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/application/image_sessions.hpp"
#include "axklib/io.hpp"

namespace {

class Tx16wCompanions : public testing::Test {
  protected:
    std::filesystem::path root = std::filesystem::temp_directory_path() / "axklib-tx16w-companions";
    void SetUp() override {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "main");
        auto setup = bytes(1536U);
        std::ranges::copy(std::string_view{"TONE"}, setup.begin() + 0xf7);
        write("main/SET.S01", setup);
        write("main/SET.V01", bytes(8704U));
        write("main/SET.F01", bytes(32U));
        auto wave = bytes(35U);
        wave[22] = static_cast<char>(0xc9);
        wave[23] = wave[24] = 2;
        write("TONE.W01", wave);
        write("OTHER.W01", wave);
        std::filesystem::create_directories(root / "duplicate");
        write("duplicate/TONE.W01", wave);
        write("bad/TONE.W01", bytes(8U));
        write("mixed/TONE.W01", wave);
        write("mixed/BAD.W02", bytes(8U));
    }
    void TearDown() override { std::filesystem::remove_all(root); }
    static std::vector<char> bytes(std::size_t size) {
        std::vector<char> result(size);
        std::ranges::copy(std::string_view{"LM8953"}, result.begin());
        return result;
    }
    void write(std::string_view path, const std::vector<char> &value) {
        std::filesystem::create_directories((root / path).parent_path());
        std::ofstream output{root / path, std::ios::binary};
        output.write(value.data(), static_cast<std::streamsize>(value.size()));
        ASSERT_TRUE(output);
    }
};

TEST_F(Tx16wCompanions, AppendingCompanionsPreservesIdsOtherDocumentsAndRevisionGuards) {
    auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, false}});
    ASSERT_TRUE(sandbox);
    axk::app::ImageSessionManager sessions{*sandbox};
    auto opened =
        sessions.open({{{"root", "main", axk::app::ImageSourceKind::directory}, {"root", "OTHER.W01"}}, {}}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    ASSERT_EQ(opened->documents.size(), 2U);
    const axk::app::ImageDocumentRef target{opened->image_id, opened->documents.front().id};
    const auto other_id = opened->documents.back().id;
    const auto before = sessions.objects(target, "owner", 100U);
    ASSERT_TRUE(before);
    EXPECT_EQ(opened->documents.front().completeness, "INCOMPLETE");
    ASSERT_EQ(opened->documents.front().companions.requirements.size(), 1U);
    EXPECT_EQ(opened->documents.front().companions.requirements.front().name, "TONE.W01");
    const axk::app::CompanionSelection selection{axk::app::CompanionSelectionKind::sources, {{"root", "TONE.W01"}}};
    auto attached = sessions.attach_companions(target, "owner", 1U, selection);
    ASSERT_TRUE(attached) << attached.error().message;
    EXPECT_EQ(attached->revision, 2U);
    EXPECT_EQ(attached->documents.front().id, target.document_id);
    EXPECT_EQ(attached->documents.back().id, other_id);
    EXPECT_EQ(attached->documents.front().completeness, "COMPLETE");
    EXPECT_TRUE(attached->documents.front().companions.requirements.empty());
    EXPECT_GT(attached->documents.front().validation.warning_count, 0U);
    EXPECT_TRUE(sessions.object_detail(target, "owner", before->items.front().id));
    EXPECT_FALSE(sessions.attach_companions(target, "owner", 1U, selection));
    EXPECT_FALSE(sessions.attach_companions(target, "other-owner", 2U, selection));
    EXPECT_FALSE(sessions.attach_companions(target, "owner", 2U, selection));
    EXPECT_EQ(sessions.inspect(target.image_id, "owner")->revision, 2U);
}

TEST_F(Tx16wCompanions, RejectsUnrelatedAmbiguousCorruptAndCancelledAttachmentsAtomically) {
    auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, false}});
    ASSERT_TRUE(sandbox);
    axk::app::ImageSessionManager sessions{*sandbox};
    const auto opened = sessions.open({{{"root", "main", axk::app::ImageSourceKind::directory}}, {}}, "owner");
    ASSERT_TRUE(opened);
    const axk::app::ImageDocumentRef target{opened->image_id, opened->documents.front().id};
    for (const auto &references : std::vector<std::vector<axk::app::ImageSourceRef>>{
             {{"root", "OTHER.W01"}},
             {{"root", "TONE.W01"}, {"root", "duplicate/TONE.W01"}},
             {{"root", "bad/TONE.W01"}},
             {{"root", "TONE.W01"}, {"root", "OTHER.W01"}},
             {{"root", "mixed", axk::app::ImageSourceKind::directory}}}) {
        EXPECT_FALSE(
            sessions.attach_companions(target, "owner", 1U, {axk::app::CompanionSelectionKind::sources, references}));
        const auto unchanged = sessions.inspect(target.image_id, "owner");
        ASSERT_TRUE(unchanged);
        EXPECT_EQ(unchanged->revision, 1U);
        EXPECT_EQ(unchanged->sources.size(), 1U);
        EXPECT_EQ(unchanged->documents.front().object_count, opened->documents.front().object_count);
    }
    axk::CancellationSource cancellation;
    cancellation.cancel();
    EXPECT_FALSE(sessions.attach_companions(target, "owner", 1U,
                                            {axk::app::CompanionSelectionKind::sources, {{"root", "TONE.W01"}}},
                                            cancellation.token()));
    EXPECT_EQ(sessions.inspect(target.image_id, "owner")->revision, 1U);
    write("main/SET.F01", bytes(40U));
    EXPECT_FALSE(sessions.attach_companions(target, "owner", 1U,
                                            {axk::app::CompanionSelectionKind::sources, {{"root", "TONE.W01"}}}));
}

TEST_F(Tx16wCompanions, NearbyDiscoveryUsesTheOwningSessionAndIgnoresUnrelatedFiles) {
    std::filesystem::copy_file(root / "main/SET.S01", root / "SET.S01");
    std::filesystem::copy_file(root / "main/SET.V01", root / "SET.V01");
    auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, false}});
    ASSERT_TRUE(sandbox);
    axk::app::ImageSessionManager sessions{*sandbox};
    const auto opened = sessions.open({{{"root", "SET.S01"}}, {}}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const axk::app::ImageDocumentRef target{opened->image_id, opened->documents.front().id};
    ASSERT_TRUE(opened->documents.front().companions.can_search_nearby);
    const auto attached =
        sessions.attach_companions(target, "owner", 1U, {axk::app::CompanionSelectionKind::immediate_siblings, {}});
    ASSERT_TRUE(attached) << attached.error().message;
    EXPECT_EQ(attached->sources.size(), 3U);
    ASSERT_EQ(attached->documents.size(), 1U);
    EXPECT_EQ(attached->documents.front().completeness, "COMPLETE");
    EXPECT_TRUE(attached->documents.front().companions.requirements.empty());
}

TEST_F(Tx16wCompanions, ExactNativeWaveCanCompleteAnUnsupportedCandidateRequirement) {
    std::vector<char> compressed(32U);
    std::ranges::copy(std::string_view{"FORM"}, compressed.begin());
    std::ranges::copy(std::string_view{"AIFC"}, compressed.begin() + 8);
    write("main/TONE.C09", compressed);
    auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, false}});
    ASSERT_TRUE(sandbox);
    axk::app::ImageSessionManager sessions{*sandbox};
    const auto opened = sessions.open({{{"root", "main", axk::app::ImageSourceKind::directory}}, {}}, "owner");
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened->documents.front().companions.requirements.size(), 1U);
    EXPECT_EQ(opened->documents.front().companions.requirements.front().state, "UNSUPPORTED");
    const axk::app::ImageDocumentRef target{opened->image_id, opened->documents.front().id};
    const auto before = sessions.objects(target, "owner", 100U);
    ASSERT_TRUE(before);
    const auto attached = sessions.attach_companions(
        target, "owner", 1U, {axk::app::CompanionSelectionKind::sources, {{"root", "TONE.W01"}}});
    ASSERT_TRUE(attached) << attached.error().message;
    EXPECT_EQ(attached->documents.front().id, target.document_id);
    EXPECT_TRUE(attached->documents.front().companions.requirements.empty());
    EXPECT_EQ(attached->documents.front().completeness, "COMPLETE");
    for (const auto &object : before->items)
        EXPECT_TRUE(sessions.object_detail(target, "owner", object.id));
}

} // namespace
