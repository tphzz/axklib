#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "axklib/application/image_sessions.hpp"
#include "axklib/media.hpp"

namespace {

std::filesystem::path fixture_path() {
    return std::filesystem::path{AXK_SOURCE_ROOT} / "tests" / "fixtures" / "images" / "sampler-authored" /
           "HD00_512_single_sbnk_authored.hds";
}

class ImageSessionTest : public testing::Test {
  protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() / "axklib-image-session-test";
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        std::filesystem::create_directories(root_);
        std::filesystem::copy_file(fixture_path(), root_ / "fixture.hds");
        auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root_, true}});
        ASSERT_TRUE(sandbox) << sandbox.error().message;
        sandbox_ = std::make_unique<axk::app::Sandbox>(std::move(*sandbox));
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    std::filesystem::path root_;
    std::unique_ptr<axk::app::Sandbox> sandbox_;
};

TEST_F(ImageSessionTest, OpensMetadataOnlySessionAndNeverExposesEngineKeysOrPaths) {
    axk::app::ImageSessionManager sessions{*sandbox_};
    const auto opened = sessions.open({"workspace", "fixture.hds"}, "owner-a");
    ASSERT_TRUE(opened) << opened.error().message;
    EXPECT_EQ(opened->source.root_id, "workspace");
    EXPECT_EQ(opened->source.relative_path, "fixture.hds");
    EXPECT_EQ(opened->format, "sfs");
    EXPECT_EQ(opened->available_operations,
              (std::vector<std::string>{"images.content", "images.objects", "images.relationships",
                                        "images.validation.issues", "images.preview", "auditions.prepare",
                                        "images.alter.volumes", "images.alter.partitions"}));
    EXPECT_GT(opened->object_count, 0U);

    const auto objects = sessions.objects(opened->image_id, "owner-a", 100U);
    ASSERT_TRUE(objects) << objects.error().message;
    ASSERT_FALSE(objects->items.empty());
    for (const auto &object : objects->items) {
        EXPECT_TRUE(object.id.starts_with("object-"));
        EXPECT_EQ(object.id.find(root_.string()), std::string::npos);
        EXPECT_EQ(object.id.find("p0:sfs"), std::string::npos);
    }
    const auto content = sessions.content(opened->image_id, "owner-a", 100U);
    ASSERT_TRUE(content) << content.error().message;
    ASSERT_FALSE(content->items.empty());
    EXPECT_EQ(content->items.front().kind, "partition");
    EXPECT_EQ(content->items.front().partition_index, 0U);
    const auto media = axk::open_media(root_ / "fixture.hds");
    ASSERT_TRUE(media) << media.error().message;
    const auto *sfs = std::get_if<axk::Container>(&media->storage());
    ASSERT_NE(sfs, nullptr);
    ASSERT_FALSE(sfs->partitions().empty());
    EXPECT_EQ(content->items.front().name, sfs->partitions().front().name);
    EXPECT_NE(content->items.front().display_name, content->items.front().name);
    for (const auto &item : content->items) {
        EXPECT_TRUE(item.id.starts_with("content-"));
        if (item.object_id) {
            EXPECT_TRUE(item.object_id->starts_with("object-"));
        }
    }
}

TEST_F(ImageSessionTest, ReportsCompleteStoredObjectSize) {
    const auto media = axk::open_media(root_ / "fixture.hds");
    ASSERT_TRUE(media) << media.error().message;
    const auto inventory = axk::build_media_inventory(*media, axk::MediaObjectReadMode::decoded_metadata);
    ASSERT_TRUE(inventory) << inventory.error().message;
    const auto catalog_object = std::ranges::find_if(
        inventory->catalog.objects, [](const auto &item) { return item.object.header.raw_type == "SMPL"; });
    ASSERT_NE(catalog_object, inventory->catalog.objects.end());
    const auto descriptor =
        std::ranges::find(inventory->objects, catalog_object->key, &axk::MediaObjectDescriptor::key);
    ASSERT_NE(descriptor, inventory->objects.end());

    axk::app::ImageSessionManager sessions{*sandbox_};
    const auto opened = sessions.open({"workspace", "fixture.hds"}, "owner-a");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto objects = sessions.objects(opened->image_id, "owner-a", 100U, std::nullopt, "SMPL");
    ASSERT_TRUE(objects) << objects.error().message;
    const auto object =
        std::ranges::find(objects->items, catalog_object->object.header.name, &axk::app::ImageObjectItem::name);
    ASSERT_NE(object, objects->items.end());
    EXPECT_EQ(object->stored_size_bytes, descriptor->size);
}

TEST_F(ImageSessionTest, HoldsPathReservationFromBeforeOpenUntilClose) {
    axk::app::PathReservationCoordinator reservations;
    axk::app::ImageSessionManager sessions{
        *sandbox_, 4U, 100U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    auto mutation = reservations.try_acquire(
        axk::app::PathAccess{{"workspace", "fixture.hds"}, axk::app::PathAccessMode::exclusive});
    ASSERT_TRUE(mutation) << mutation.error().message;
    const auto blocked = sessions.open({"workspace", "fixture.hds"}, "owner-a");
    ASSERT_FALSE(blocked);
    EXPECT_EQ(blocked.error().code, "entry_in_use");
    mutation = {};

    const auto opened = sessions.open({"workspace", "fixture.hds"}, "owner-a");
    ASSERT_TRUE(opened) << opened.error().message;
    EXPECT_FALSE(
        reservations.try_acquire(axk::app::PathAccess{{"workspace", ""}, axk::app::PathAccessMode::exclusive}));
    ASSERT_TRUE(sessions.close(opened->image_id, "owner-a"));
    EXPECT_TRUE(reservations.try_acquire(
        axk::app::PathAccess{{"workspace", "fixture.hds"}, axk::app::PathAccessMode::exclusive}));
}

TEST_F(ImageSessionTest, PagesDeterministicallyAndRejectsForeignOrInvalidCursors) {
    axk::app::ImageSessionManager sessions{*sandbox_, 4U, 2U};
    const auto opened = sessions.open({"workspace", "fixture.hds"}, "owner-a");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto first = sessions.objects(opened->image_id, "owner-a", 2U);
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_EQ(first->items.size(), 2U);
    ASSERT_TRUE(first->next_cursor);
    const auto repeated = sessions.objects(opened->image_id, "owner-a", 2U);
    ASSERT_TRUE(repeated);
    EXPECT_EQ(repeated->next_cursor, first->next_cursor);
    EXPECT_EQ(repeated->items.front().id, first->items.front().id);
    const auto second = sessions.objects(opened->image_id, "owner-a", 2U, *first->next_cursor);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_NE(second->items.front().id, first->items.front().id);

    const auto invalid = sessions.objects(opened->image_id, "owner-a", 2U, "cursor-invalid");
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, "invalid_cursor");
    const auto foreign = sessions.objects(opened->image_id, "owner-b", 2U);
    ASSERT_FALSE(foreign);
    EXPECT_EQ(foreign.error().code, "image_not_found");
}

TEST_F(ImageSessionTest, FiltersObjectsAndBindsCursorsToTheObjectType) {
    axk::app::ImageSessionManager sessions{*sandbox_, 4U, 1U};
    const auto opened = sessions.open({"workspace", "fixture.hds"}, "owner-a");
    ASSERT_TRUE(opened) << opened.error().message;

    const auto waveforms = sessions.objects(opened->image_id, "owner-a", 1U, std::nullopt, "SMPL");
    ASSERT_TRUE(waveforms) << waveforms.error().message;
    ASSERT_FALSE(waveforms->items.empty());
    EXPECT_TRUE(std::ranges::all_of(waveforms->items, [](const auto &item) { return item.type == "SMPL"; }));

    const auto missing = sessions.objects(opened->image_id, "owner-a", 1U, std::nullopt, "MISSING");
    ASSERT_TRUE(missing) << missing.error().message;
    EXPECT_TRUE(missing->items.empty());
    EXPECT_EQ(missing->total_count, 0U);

    if (waveforms->next_cursor) {
        const auto wrong_filter = sessions.objects(opened->image_id, "owner-a", 1U, *waveforms->next_cursor, "SBNK");
        ASSERT_FALSE(wrong_filter);
        EXPECT_EQ(wrong_filter.error().code, "invalid_cursor");
    }
}

TEST_F(ImageSessionTest, FiltersObjectsByContentScopeAndBindsCursorsToTheScope) {
    axk::app::ImageSessionManager sessions{*sandbox_, 4U, 1U};
    const auto opened = sessions.open({"workspace", "fixture.hds"}, "owner-a");
    ASSERT_TRUE(opened) << opened.error().message;

    const auto roots = sessions.content(opened->image_id, "owner-a", 1U);
    ASSERT_TRUE(roots) << roots.error().message;
    ASSERT_FALSE(roots->items.empty());
    const auto volumes = sessions.content(opened->image_id, "owner-a", 1U, std::nullopt, roots->items.front().id);
    ASSERT_TRUE(volumes) << volumes.error().message;
    const auto volume = std::ranges::find(volumes->items, "volume", &axk::app::ImageContentItem::kind);
    ASSERT_NE(volume, volumes->items.end());
    EXPECT_EQ(volume->partition_index, roots->items.front().partition_index);

    const auto waveforms = sessions.objects(opened->image_id, "owner-a", 1U, std::nullopt, "SMPL", volume->id);
    ASSERT_TRUE(waveforms) << waveforms.error().message;
    ASSERT_FALSE(waveforms->items.empty());
    EXPECT_TRUE(std::ranges::all_of(waveforms->items, [&](const auto &item) {
        return item.type == "SMPL" && item.volume_name == volume->display_name;
    }));

    const auto missing =
        sessions.objects(opened->image_id, "owner-a", 1U, std::nullopt, std::nullopt, "content-missing");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, "content_not_found");

    const auto scoped = sessions.objects(opened->image_id, "owner-a", 1U, std::nullopt, std::nullopt, volume->id);
    ASSERT_TRUE(scoped) << scoped.error().message;
    ASSERT_TRUE(scoped->next_cursor);
    const auto wrong_type = sessions.objects(opened->image_id, "owner-a", 1U, *scoped->next_cursor, "SMPL", volume->id);
    ASSERT_FALSE(wrong_type);
    EXPECT_EQ(wrong_type.error().code, "invalid_cursor");
    const auto wrong_scope =
        sessions.objects(opened->image_id, "owner-a", 1U, *scoped->next_cursor, std::nullopt, roots->items.front().id);
    ASSERT_FALSE(wrong_scope);
    EXPECT_EQ(wrong_scope.error().code, "invalid_cursor");
}

TEST_F(ImageSessionTest, FiltersRelationshipsByVolumeObjectAndTypeAndBindsCursorsToTheFilter) {
    axk::app::ImageSessionManager sessions{*sandbox_, 4U, 100U};
    const auto opened = sessions.open({"workspace", "fixture.hds"}, "owner-a");
    ASSERT_TRUE(opened) << opened.error().message;

    const auto roots = sessions.content(opened->image_id, "owner-a", 1U);
    ASSERT_TRUE(roots) << roots.error().message;
    const auto volumes = sessions.content(opened->image_id, "owner-a", 100U, std::nullopt, roots->items.front().id);
    ASSERT_TRUE(volumes) << volumes.error().message;
    const auto volume = std::ranges::find(volumes->items, "volume", &axk::app::ImageContentItem::kind);
    ASSERT_NE(volume, volumes->items.end());

    const auto scoped = sessions.relationships(opened->image_id, "owner-a", 1U, std::nullopt,
                                               {.content_scope_id = volume->id,
                                                .source_object_id = std::nullopt,
                                                .target_object_id = std::nullopt,
                                                .relationship_type = std::nullopt});
    ASSERT_TRUE(scoped) << scoped.error().message;
    ASSERT_FALSE(scoped->items.empty());
    const auto &first = scoped->items.front();

    const auto combined = sessions.relationships(
        opened->image_id, "owner-a", 100U, std::nullopt,
        {.content_scope_id = volume->id,
         .source_object_id = first.source_object_id,
         .target_object_id =
             first.target_object_id ? std::optional<std::string_view>{*first.target_object_id} : std::nullopt,
         .relationship_type = first.type});
    ASSERT_TRUE(combined) << combined.error().message;
    ASSERT_FALSE(combined->items.empty());
    EXPECT_TRUE(std::ranges::all_of(combined->items, [&](const auto &item) {
        return item.source_object_id == first.source_object_id && item.target_object_id == first.target_object_id &&
               item.type == first.type;
    }));

    if (scoped->next_cursor) {
        const auto wrong_filter = sessions.relationships(opened->image_id, "owner-a", 1U, *scoped->next_cursor,
                                                         {.content_scope_id = volume->id,
                                                          .source_object_id = std::nullopt,
                                                          .target_object_id = std::nullopt,
                                                          .relationship_type = first.type});
        ASSERT_FALSE(wrong_filter);
        EXPECT_EQ(wrong_filter.error().code, "invalid_cursor");
    }

    const auto missing = sessions.relationships(opened->image_id, "owner-a", 1U, std::nullopt,
                                                {.content_scope_id = "content-missing",
                                                 .source_object_id = std::nullopt,
                                                 .target_object_id = std::nullopt,
                                                 .relationship_type = std::nullopt});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, "content_not_found");
}

TEST_F(ImageSessionTest, PagesContentByParentAndBindsCursorsToThatParent) {
    axk::app::ImageSessionManager sessions{*sandbox_, 4U, 2U};
    const auto opened = sessions.open({"workspace", "fixture.hds"}, "owner-a");
    ASSERT_TRUE(opened) << opened.error().message;

    const auto roots = sessions.content(opened->image_id, "owner-a", 2U);
    ASSERT_TRUE(roots) << roots.error().message;
    ASSERT_FALSE(roots->items.empty());
    for (const auto &item : roots->items)
        EXPECT_FALSE(item.parent_id);

    const auto parent_id = roots->items.front().id;
    const auto children = sessions.content(opened->image_id, "owner-a", 2U, std::nullopt, parent_id);
    ASSERT_TRUE(children) << children.error().message;
    ASSERT_FALSE(children->items.empty());
    for (const auto &item : children->items)
        EXPECT_EQ(item.parent_id, parent_id);

    const auto missing = sessions.content(opened->image_id, "owner-a", 2U, std::nullopt, "content-missing");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, "content_not_found");

    if (children->next_cursor) {
        const auto wrong_parent = sessions.content(opened->image_id, "owner-a", 2U, *children->next_cursor);
        ASSERT_FALSE(wrong_parent);
        EXPECT_EQ(wrong_parent.error().code, "invalid_cursor");
    }
}

TEST_F(ImageSessionTest, ExpiresSessionsAndKeepsCloseIdempotent) {
    auto now = std::chrono::steady_clock::now();
    axk::app::ImageSessionManager sessions{*sandbox_, 2U, 100U, std::chrono::seconds{5}, [&] { return now; }};
    const auto opened = sessions.open({"workspace", "fixture.hds"}, "owner-a");
    ASSERT_TRUE(opened) << opened.error().message;
    now += std::chrono::seconds{6};
    const auto expired = sessions.inspect(opened->image_id, "owner-a");
    ASSERT_FALSE(expired);
    EXPECT_EQ(expired.error().code, "image_not_found");
    EXPECT_TRUE(sessions.close(opened->image_id, "owner-a"));
    EXPECT_TRUE(sessions.close("image-does-not-exist", "owner-a"));
}

TEST_F(ImageSessionTest, EnforcesCapacityAndSurvivesCloseDuringAnExistingPage) {
    axk::app::ImageSessionManager sessions{*sandbox_, 1U};
    const auto opened = sessions.open({"workspace", "fixture.hds"}, "owner-a");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto full = sessions.open({"workspace", "fixture.hds"}, "owner-a");
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error().code, "image_capacity_exhausted");
    EXPECT_TRUE(full.error().retryable);
    const auto page = sessions.content(opened->image_id, "owner-a", 100U);
    ASSERT_TRUE(page);
    ASSERT_TRUE(sessions.close(opened->image_id, "owner-a"));
    EXPECT_FALSE(page->items.empty());
    EXPECT_FALSE(sessions.inspect(opened->image_id, "owner-a"));
}

TEST_F(ImageSessionTest, BuildsBoundedPreviewForOpaqueWaveformIdentifier) {
    axk::app::ImageSessionManager sessions{*sandbox_, 2U, 64U};
    const auto opened = sessions.open({"workspace", "fixture.hds"}, "owner-a");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto objects = sessions.objects(opened->image_id, "owner-a", 64U);
    ASSERT_TRUE(objects);
    const auto waveform = std::ranges::find(objects->items, "SMPL", &axk::app::ImageObjectItem::type);
    ASSERT_NE(waveform, objects->items.end());
    const auto preview = sessions.preview(opened->image_id, "owner-a", waveform->id, 32U);
    ASSERT_TRUE(preview) << preview.error().message;
    EXPECT_EQ(preview->object_id, waveform->id);
    EXPECT_EQ(preview->bins.size(), 32U);
    EXPECT_GT(preview->frame_count, 0U);
    EXPECT_TRUE(sessions.preview(opened->image_id, "owner-a", waveform->id, 1024U));
    EXPECT_FALSE(sessions.preview(opened->image_id, "owner-a", waveform->id, 4097U));
    EXPECT_FALSE(sessions.preview(opened->image_id, "owner-a", "object-unknown", 32U));
}

TEST_F(ImageSessionTest, PreparesOwnerBoundRangeReadableWavAndInvalidatesItWithTheImage) {
    axk::app::ImageSessionManager sessions{*sandbox_, 2U, 64U};
    const auto opened = sessions.open({"workspace", "fixture.hds"}, "owner-a");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto objects = sessions.objects(opened->image_id, "owner-a", 64U, std::nullopt, "SMPL");
    ASSERT_TRUE(objects);
    ASSERT_FALSE(objects->items.empty());

    const auto audition = sessions.prepare_audition(opened->image_id, "owner-a", objects->items.front().id);
    ASSERT_TRUE(audition) << audition.error().message;
    EXPECT_EQ(audition->channels, 1U);
    EXPECT_GT(audition->sample_rate, 0U);
    EXPECT_GT(audition->frame_count, 0U);
    EXPECT_GT(audition->wav_size_bytes, 44U);

    const auto header = sessions.audition_range(audition->audition_id, "owner-a", 0U, 44U);
    ASSERT_TRUE(header) << header.error().message;
    EXPECT_EQ(header->total_size, audition->wav_size_bytes);
    ASSERT_EQ(header->bytes.size(), 44U);
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(header->bytes.data()), 4U), "RIFF");
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(header->bytes.data() + 8U), 4U), "WAVE");

    const auto crossing = sessions.audition_range(audition->audition_id, "owner-a", 40U, 16U);
    ASSERT_TRUE(crossing) << crossing.error().message;
    EXPECT_EQ(crossing->bytes.size(), 16U);
    EXPECT_FALSE(sessions.audition_range(audition->audition_id, "owner-b", 0U, 1U));
    EXPECT_FALSE(sessions.audition_range(audition->audition_id, "owner-a", audition->wav_size_bytes, 1U));

    ASSERT_TRUE(sessions.close(opened->image_id, "owner-a"));
    EXPECT_FALSE(sessions.audition_range(audition->audition_id, "owner-a", 0U, 1U));
}

TEST_F(ImageSessionTest, PreparesSampleAuditionFromConfirmedLinkedWaveData) {
    axk::app::ImageSessionManager sessions{*sandbox_, 2U, 64U};
    const auto opened = sessions.open({"workspace", "fixture.hds"}, "owner-a");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto objects = sessions.objects(opened->image_id, "owner-a", 64U, std::nullopt, "SBNK");
    ASSERT_TRUE(objects);
    ASSERT_FALSE(objects->items.empty());

    const auto audition = sessions.prepare_audition(opened->image_id, "owner-a", objects->items.front().id);
    ASSERT_TRUE(audition) << audition.error().message;
    EXPECT_EQ(audition->channels, 1U);
    EXPECT_GT(audition->frame_count, 0U);
    const auto header = sessions.audition_range(audition->audition_id, "owner-a", 0U, 44U);
    ASSERT_TRUE(header) << header.error().message;
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(header->bytes.data() + 8U), 4U), "WAVE");
}

TEST_F(ImageSessionTest, ActiveAuditionRangesKeepTheOwningImageSessionAlive) {
    auto now = std::chrono::steady_clock::now();
    axk::app::ImageSessionManager sessions{*sandbox_, 2U, 64U, std::chrono::seconds{5}, [&] { return now; }};
    const auto opened = sessions.open({"workspace", "fixture.hds"}, "owner-a");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto objects = sessions.objects(opened->image_id, "owner-a", 64U, std::nullopt, "SMPL");
    ASSERT_TRUE(objects);
    ASSERT_FALSE(objects->items.empty());
    const auto audition = sessions.prepare_audition(opened->image_id, "owner-a", objects->items.front().id);
    ASSERT_TRUE(audition) << audition.error().message;

    now += std::chrono::seconds{4};
    ASSERT_TRUE(sessions.audition_range(audition->audition_id, "owner-a", 0U, 44U));
    now += std::chrono::seconds{4};
    EXPECT_TRUE(sessions.inspect(opened->image_id, "owner-a"));
}

} // namespace
