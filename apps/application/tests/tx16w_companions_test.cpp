#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
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
    static std::vector<char> odd_wave(bool looped, bool known_rate = true) {
        auto result = bytes(41U);
        result[22] = static_cast<char>(looped ? 0x49 : 0xc9);
        result[23] = known_rate ? 2 : 0;
        result[24] = 2;
        result[27] = 3;
        // Five signed 12-bit samples; the sixth packed sample is padding.
        const std::array<unsigned char, 9> packed{0x80, 0x0f, 0xff, 0x00, 0x01, 0x00, 0x7f, 0xf0, 0x80};
        std::ranges::transform(packed, result.begin() + 32,
                               [](unsigned char value) { return static_cast<char>(value); });
        return result;
    }
    void write(std::string_view path, const std::vector<char> &value) {
        std::filesystem::create_directories((root / path).parent_path());
        std::ofstream output{root / path, std::ios::binary};
        output.write(value.data(), static_cast<std::streamsize>(value.size()));
        ASSERT_TRUE(output);
    }
};

TEST_F(Tx16wCompanions, WaveNamesAgreeAcrossCatalogDetailsAndAudioWithoutLosingFileNavigation) {
    write("catalog/nested/Tone.W01", odd_wave(true));
    const auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, false}});
    ASSERT_TRUE(sandbox);
    axk::app::ImageSessionManager sessions{*sandbox};
    const auto opened = sessions.open({{{"root", "catalog", axk::app::ImageSourceKind::directory}}, {}}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    ASSERT_EQ(opened->documents.size(), 1U);
    const axk::app::ImageDocumentRef target{opened->image_id, opened->documents.front().id};
    const auto objects = sessions.objects(target, "owner", 100U);
    ASSERT_TRUE(objects);
    ASSERT_EQ(objects->items.size(), 1U);
    const auto &object = objects->items.front();
    EXPECT_EQ(object.name, "Tone.W01");
    const auto detail = sessions.object_detail(target, "owner", object.id);
    ASSERT_TRUE(detail) << detail.error().message;
    EXPECT_EQ(detail->at("name"), object.name);
    EXPECT_EQ(detail->at("locations").at(0).at("entryId"), "nested/Tone.W01");
    const auto files = sessions.filesystem(opened->image_id, "owner", opened->revision,
                                           {.object_id = object.id, .document_id = target.document_id});
    ASSERT_TRUE(files) << files.error().message;
    ASSERT_EQ(files->items.size(), 1U);
    EXPECT_EQ(files->items.front().path, "nested/Tone.W01");
    EXPECT_EQ(files->items.front().name, object.name);
    EXPECT_EQ(files->items.front().source_member_id, opened->sources.front().id);
    ASSERT_EQ(files->items.front().bindings.size(), 1U);
    EXPECT_EQ(files->items.front().bindings.front().document_id, target.document_id);
    EXPECT_EQ(files->items.front().bindings.front().object_id, std::optional<std::string>{object.id});
    const auto preview = sessions.preview(target, "owner", object.id, 5U, opened->revision);
    ASSERT_TRUE(preview) << preview.error().message;
    EXPECT_EQ(preview->object_id, object.id);
    EXPECT_EQ(preview->document_id, target.document_id);
    EXPECT_EQ(preview->revision, opened->revision);
}

TEST_F(Tx16wCompanions, ListsAndInspectsEverySameNamedPerformanceAcrossPages) {
    auto performances = bytes(5120U);
    for (std::size_t slot = 0U; slot < 32U; ++slot) {
        const auto offset = 16U + slot * 146U;
        std::ranges::copy(std::string_view{"init performance"}, performances.data() + offset + 0x78U);
        if (slot == 12U)
            performances[offset + 66U] = 99;
    }
    write("catalog/SET.U01", performances);
    const auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, false}});
    ASSERT_TRUE(sandbox);
    axk::app::ImageSessionManager sessions{*sandbox};
    const auto opened = sessions.open({{{"root", "catalog/SET.U01"}}, {}}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    ASSERT_EQ(opened->documents.size(), 1U);
    const axk::app::ImageDocumentRef target{opened->image_id, opened->documents.front().id};
    std::optional<std::string> cursor;
    std::set<std::string> ids;
    std::set<std::uint8_t> slots;
    do {
        const auto page = sessions.objects(target, "owner", 7U, cursor, "performance");
        ASSERT_TRUE(page) << page.error().message;
        EXPECT_EQ(page->total_count, 32U);
        for (const auto &object : page->items) {
            EXPECT_EQ(object.name, "init performance");
            ASSERT_TRUE(ids.insert(object.id).second);
            const auto detail = sessions.object_detail(target, "owner", object.id);
            ASSERT_TRUE(detail) << detail.error().message;
            EXPECT_EQ(detail->at("name"), object.name);
            EXPECT_EQ(detail->at("objectId"), object.id);
            const auto slot = detail->at("details").at("slot").get<std::uint8_t>();
            EXPECT_LT(slot, std::uint8_t{32U});
            EXPECT_TRUE(slots.insert(slot).second);
            EXPECT_EQ(detail->at("details").at("voices").at(0).at("volume"), slot == 12U ? 99U : 0U);
            EXPECT_EQ(detail->at("locations").at(0).at("offset"), 16U + slot * 146U);
            EXPECT_EQ(detail->at("locations").at(0).at("length"), 146U);
        }
        cursor = page->next_cursor;
    } while (cursor);
    EXPECT_EQ(ids.size(), 32U);
    EXPECT_EQ(slots.size(), 32U);
}

TEST_F(Tx16wCompanions, PreviewsOddPackedWaveWithExactAmplitudeAndAttackRepeatLoopBounds) {
    write("TONE.W01", odd_wave(true));
    const auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, false}});
    ASSERT_TRUE(sandbox);
    axk::app::ImageSessionManager sessions{*sandbox};
    const auto opened = sessions.open({{{"root", "TONE.W01"}}, {}}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const axk::app::ImageDocumentRef target{opened->image_id, opened->documents.front().id};
    const auto objects = sessions.objects(target, "owner", 100U);
    ASSERT_TRUE(objects);
    ASSERT_EQ(objects->items.size(), 1U);
    const auto preview = sessions.preview(target, "owner", objects->items.front().id, 5U, opened->revision);
    ASSERT_TRUE(preview) << preview.error().message;
    EXPECT_EQ(preview->image_id, opened->image_id);
    EXPECT_EQ(preview->document_id, target.document_id);
    EXPECT_EQ(preview->revision, opened->revision);
    EXPECT_TRUE(preview->warnings.empty());
    ASSERT_EQ(preview->lanes.size(), 1U);
    const auto &lane = preview->lanes.front();
    EXPECT_EQ(lane.source_object_id, objects->items.front().id);
    EXPECT_EQ(lane.sample_rate, 50'000U);
    EXPECT_EQ(lane.sample_width_bytes, 2U);
    EXPECT_EQ(lane.stored_frame_count, 5U);
    EXPECT_EQ(lane.playback_start_frame, 0U);
    EXPECT_EQ(lane.playback_length_frames, 5U);
    EXPECT_EQ(lane.loop_start_frame, 2U);
    EXPECT_EQ(lane.loop_length_frames, 3U);
    const std::array<std::int32_t, 5> expected{-32768, -16, 0, 16, 32752};
    ASSERT_EQ(lane.bins.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(lane.bins[index].minimum, expected[index]);
        EXPECT_EQ(lane.bins[index].maximum, expected[index]);
    }
}

TEST_F(Tx16wCompanions, AuditionsOddPackedWaveWithoutPaddingAndWarnsAboutUnknownRate) {
    const auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, false}});
    ASSERT_TRUE(sandbox);
    axk::app::ImageSessionManager sessions{*sandbox};
    for (const bool looped : {false, true}) {
        SCOPED_TRACE(looped);
        const auto filename = looped ? "LOOP.W01" : "ONCE.W01";
        write(filename, odd_wave(looped, false));
        const auto opened = sessions.open({{{"root", filename}}, {}}, "owner");
        ASSERT_TRUE(opened) << opened.error().message;
        const axk::app::ImageDocumentRef target{opened->image_id, opened->documents.front().id};
        const auto objects = sessions.objects(target, "owner", 100U);
        ASSERT_TRUE(objects);
        ASSERT_EQ(objects->items.size(), 1U);
        const auto audition = sessions.prepare_audition(target, "owner", {objects->items.front().id}, opened->revision);
        ASSERT_TRUE(audition) << audition.error().message;
        EXPECT_EQ(audition->image_id, opened->image_id);
        EXPECT_EQ(audition->document_id, target.document_id);
        EXPECT_EQ(audition->revision, opened->revision);
        ASSERT_EQ(audition->clips.size(), 1U);
        const auto &clip = audition->clips.front();
        EXPECT_EQ(clip.direction, "FORWARD");
        EXPECT_EQ(clip.looping, looped);
        EXPECT_FALSE(clip.warnings.empty());
        const auto preview = sessions.preview(target, "owner", objects->items.front().id, 5U, opened->revision);
        ASSERT_TRUE(preview) << preview.error().message;
        EXPECT_FALSE(preview->warnings.empty());
        ASSERT_EQ(clip.lanes.size(), 1U);
        const auto &lane = clip.lanes.front();
        EXPECT_EQ(lane.sample_rate, 33'333U);
        EXPECT_EQ(lane.sample_width_bytes, 2U);
        EXPECT_EQ(lane.frame_count, 5U);
        EXPECT_EQ(lane.loop_start_frame, looped ? 2U : 0U);
        EXPECT_EQ(lane.loop_length_frames, looped ? 3U : 0U);
        EXPECT_EQ(lane.wav_size_bytes, 54U);
        const auto pcm = sessions.audition_range(audition->audition_id, "owner", lane.content_offset_bytes + 44U, 10U);
        ASSERT_TRUE(pcm) << pcm.error().message;
        const std::vector<std::byte> expected{std::byte{0x00}, std::byte{0x80}, std::byte{0xf0}, std::byte{0xff},
                                              std::byte{0x00}, std::byte{0x00}, std::byte{0x10}, std::byte{0x00},
                                              std::byte{0xf0}, std::byte{0x7f}};
        EXPECT_EQ(pcm->bytes, expected);
        EXPECT_TRUE(sessions.delete_audition(audition->audition_id, "owner"));
    }
}

TEST_F(Tx16wCompanions, AudioRequestsEnforceRevisionOwnershipCancellationAndRangeBoundaries) {
    write("TONE.W01", odd_wave(true));
    const auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, false}});
    ASSERT_TRUE(sandbox);
    axk::app::ImageSessionManager sessions{*sandbox};
    const auto opened = sessions.open({{{"root", "TONE.W01"}}, {}}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const axk::app::ImageDocumentRef target{opened->image_id, opened->documents.front().id};
    const auto objects = sessions.objects(target, "owner", 100U);
    ASSERT_TRUE(objects);
    ASSERT_EQ(objects->items.size(), 1U);
    const auto &object_id = objects->items.front().id;
    EXPECT_FALSE(sessions.preview(target, "other-owner", object_id, 5U, opened->revision));
    EXPECT_FALSE(sessions.prepare_audition(target, "other-owner", {object_id}, opened->revision));
    EXPECT_FALSE(sessions.preview(target, "owner", object_id, 0U, opened->revision));
    EXPECT_FALSE(sessions.preview(target, "owner", object_id, 4097U, opened->revision));
    axk::CancellationSource cancelled;
    cancelled.cancel();
    EXPECT_FALSE(sessions.preview(target, "owner", object_id, 5U, opened->revision, cancelled.token()));
    EXPECT_FALSE(sessions.prepare_audition(target, "owner", {object_id}, opened->revision, cancelled.token()));
    const auto audition = sessions.prepare_audition(target, "owner", {object_id}, opened->revision);
    ASSERT_TRUE(audition) << audition.error().message;
    EXPECT_FALSE(sessions.audition_range(audition->audition_id, "other-owner", 44U, 10U));
    EXPECT_FALSE(sessions.audition_range(audition->audition_id, "owner", 44U, 10U, cancelled.token()));
    EXPECT_FALSE(sessions.audition_range(audition->audition_id, "owner", audition->content_size_bytes, 1U));
    EXPECT_FALSE(
        sessions.audition_range(audition->audition_id, "owner", std::numeric_limits<std::uint64_t>::max(), 1U));
    const auto suffix = sessions.audition_range(audition->audition_id, "owner", 51U, 3U);
    ASSERT_TRUE(suffix) << suffix.error().message;
    EXPECT_EQ(suffix->bytes, (std::vector<std::byte>{std::byte{0x00}, std::byte{0xf0}, std::byte{0x7f}}));
    const auto attached = sessions.attach_companions(
        target, "owner", opened->revision,
        {axk::app::CompanionSelectionKind::sources, {{"root", "main", axk::app::ImageSourceKind::directory}}});
    ASSERT_TRUE(attached) << attached.error().message;
    const auto stale_preview = sessions.preview(target, "owner", object_id, 5U, opened->revision);
    ASSERT_FALSE(stale_preview);
    EXPECT_EQ(stale_preview.error().code, "image_revision_stale");
    const auto stale_audition = sessions.prepare_audition(target, "owner", {object_id}, opened->revision);
    ASSERT_FALSE(stale_audition);
    EXPECT_EQ(stale_audition.error().code, "image_revision_stale");
    EXPECT_FALSE(sessions.audition_range(audition->audition_id, "owner", 44U, 10U));
    const auto fresh_preview = sessions.preview(target, "owner", object_id, 5U, attached->revision);
    ASSERT_TRUE(fresh_preview) << fresh_preview.error().message;
    EXPECT_EQ(fresh_preview->revision, attached->revision);
    const auto fresh_audition = sessions.prepare_audition(target, "owner", {object_id}, attached->revision);
    ASSERT_TRUE(fresh_audition) << fresh_audition.error().message;
    EXPECT_EQ(fresh_audition->revision, attached->revision);
    EXPECT_TRUE(sessions.delete_audition(fresh_audition->audition_id, "owner"));
}

TEST_F(Tx16wCompanions, OpaqueNativeFilesHaveNoDeviceNavigationBinding) {
    write("SET.F01", bytes(32U));
    const auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, false}});
    ASSERT_TRUE(sandbox);
    axk::app::ImageSessionManager sessions{*sandbox};
    const auto opened = sessions.open({{{"root", "TONE.W01"}, {"root", "SET.F01"}}, {}}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto roots = sessions.filesystem(opened->image_id, "owner", opened->revision);
    ASSERT_TRUE(roots) << roots.error().message;
    const auto opaque_root =
        std::ranges::find(roots->items, opened->sources.back().id, &axk::app::ImageFilesystemEntry::source_member_id);
    ASSERT_NE(opaque_root, roots->items.end());
    const auto files = sessions.filesystem(opened->image_id, "owner", opened->revision, {.parent_id = opaque_root->id});
    ASSERT_TRUE(files) << files.error().message;
    ASSERT_EQ(files->items.size(), 1U);
    EXPECT_TRUE(files->items.front().bindings.empty());
    EXPECT_FALSE(files->items.front().object_id);
    EXPECT_FALSE(files->items.front().content_scope_id);
}

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
