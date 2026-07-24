#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>

#include <gtest/gtest.h>

#include "axklib/application/image_sessions.hpp"
#include "axklib/media.hpp"

namespace {

std::filesystem::path fixture_path() {
    return std::filesystem::path{AXK_SOURCE_ROOT} / "tests" / "fixtures" / "images" / "sampler-authored" /
           "HD00_512_single_sbnk_authored.hds";
}

void patch_sample_cached_reference(const std::filesystem::path &path, std::uint32_t value) {
    const auto media = axk::open_media(path);
    ASSERT_TRUE(media) << media.error().message;
    const auto *sfs = std::get_if<axk::Container>(&media->storage());
    ASSERT_NE(sfs, nullptr);
    ASSERT_FALSE(sfs->partitions().empty());
    const auto &partition = sfs->partitions().front();
    const auto sample =
        std::ranges::find_if(partition.records, [](const auto &record) { return record.object_type == "SBNK"; });
    ASSERT_NE(sample, partition.records.end());
    ASSERT_EQ(sample->extents.size(), 1U);
    const auto absolute =
        (static_cast<std::uint64_t>(partition.start_sector) +
         static_cast<std::uint64_t>(sample->extents.front().cluster_offset) * partition.sectors_per_cluster) *
            512U +
        0xa0U;
    const std::array bytes{static_cast<char>(value >> 24U), static_cast<char>(value >> 16U),
                           static_cast<char>(value >> 8U), static_cast<char>(value)};
    std::fstream image{path, std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(image);
    image.seekp(static_cast<std::streamoff>(absolute));
    image.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(image);
}

void patch_sample_window(const std::filesystem::path &path, std::uint32_t first_frame, std::uint32_t frame_count,
                         std::uint32_t loop_start, std::uint32_t loop_length,
                         std::optional<std::array<std::uint32_t, 4>> right_window = std::nullopt) {
    const auto media = axk::open_media(path);
    ASSERT_TRUE(media) << media.error().message;
    const auto *sfs = std::get_if<axk::Container>(&media->storage());
    ASSERT_NE(sfs, nullptr);
    ASSERT_FALSE(sfs->partitions().empty());
    const auto &partition = sfs->partitions().front();
    const auto sample =
        std::ranges::find_if(partition.records, [](const auto &record) { return record.object_type == "SBNK"; });
    ASSERT_NE(sample, partition.records.end());
    ASSERT_EQ(sample->extents.size(), 1U);
    const auto absolute =
        (static_cast<std::uint64_t>(partition.start_sector) +
         static_cast<std::uint64_t>(sample->extents.front().cluster_offset) * partition.sectors_per_cluster) *
        512U;
    std::fstream image{path, std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(image);
    const auto write_be32 = [&](std::uint64_t offset, std::uint32_t value) {
        const std::array bytes{static_cast<char>(value >> 24U), static_cast<char>(value >> 16U),
                               static_cast<char>(value >> 8U), static_cast<char>(value)};
        image.seekp(static_cast<std::streamoff>(absolute + offset));
        image.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(image);
    };
    const auto write_be16 = [&](std::uint64_t offset, std::uint16_t value) {
        const std::array bytes{static_cast<char>(value >> 8U), static_cast<char>(value)};
        image.seekp(static_cast<std::streamoff>(absolute + offset));
        image.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(image);
    };
    const auto write_u8 = [&](std::uint64_t offset, std::uint8_t value) {
        image.seekp(static_cast<std::streamoff>(absolute + offset));
        image.put(static_cast<char>(value));
        ASSERT_TRUE(image);
    };
    write_be32(0xe8U, first_frame);
    write_be32(0xf0U, frame_count);
    write_be32(0xf8U, loop_start);
    write_be32(0x100U, loop_length);
    if (right_window) {
        std::array<char, 16> name{};
        constexpr std::string_view wave_name = "sine wave";
        std::ranges::copy(wave_name, name.begin());
        image.seekp(static_cast<std::streamoff>(absolute + 0x88U));
        image.write(name.data(), static_cast<std::streamsize>(name.size()));
        ASSERT_TRUE(image);
        write_be32(0xa4U, 23'797'180U);
        write_u8(0xd7U, 66U);
        write_be16(0xdaU, 48'000U);
        write_u8(0xddU, static_cast<std::uint8_t>(-20));
        write_be16(0xe0U, 5'442U);
        write_be32(0xecU, (*right_window)[0]);
        write_be32(0xf4U, (*right_window)[1]);
        write_be32(0xfcU, (*right_window)[2]);
        write_be32(0x104U, (*right_window)[3]);
    }
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
                                        "images.alter.volumes", "images.alter.partitions", "images.alter.objects"}));
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

TEST_F(ImageSessionTest, PlansOpaqueIdDeletionWithOptionalWaveDataCleanup) {
    axk::app::ImageSessionManager sessions{*sandbox_, 2U, 64U};
    const auto opened = sessions.open({"workspace", "fixture.hds"}, "owner-a");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto samples = sessions.objects(opened->image_id, "owner-a", 64U, std::nullopt, "SBNK");
    ASSERT_TRUE(samples) << samples.error().message;
    const auto sample = std::ranges::find(samples->items, "sine wave", &axk::app::ImageObjectItem::name);
    ASSERT_NE(sample, samples->items.end());

    const auto inspected = sessions.plan_deletion(opened->image_id, "owner-a", opened->revision, sample->id, {});

    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected->inspection.valid);
    EXPECT_EQ(inspected->inspection.target_object_id, sample->id);
    ASSERT_EQ(inspected->manifest.operations.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<axk::DeleteSampleOperation>(inspected->manifest.operations.front().data));
    const auto optional_wave = std::ranges::find(inspected->inspection.impacts, std::string{"SMPL"},
                                                 &axk::app::ImageObjectDeletionImpact::object_type);
    ASSERT_NE(optional_wave, inspected->inspection.impacts.end());
    EXPECT_EQ(optional_wave->status, "OPTIONAL");
    EXPECT_FALSE(optional_wave->selected);

    const auto selected =
        sessions.plan_deletion(opened->image_id, "owner-a", opened->revision, sample->id, {optional_wave->object_id});
    ASSERT_TRUE(selected) << selected.error().message;
    ASSERT_EQ(selected->manifest.operations.size(), 2U);
    EXPECT_GT(selected->inspection.estimated_freed_bytes, 0U);

    const auto stale = sessions.plan_deletion(opened->image_id, "owner-a", opened->revision + 1U, sample->id, {});
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, "image_revision_stale");
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

TEST_F(ImageSessionTest, MutationAdmissionUpgradesAndAbortRestoresTheSessionLease) {
    axk::app::PathReservationCoordinator reservations;
    axk::app::ImageSessionManager sessions{
        *sandbox_, 4U, 100U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "fixture.hds"}, "owner-a");
    ASSERT_TRUE(opened) << opened.error().message;
    EXPECT_EQ(opened->revision, 1U);
    const auto mutation = sessions.begin_mutation(opened->image_id, "owner-a", opened->revision);
    ASSERT_TRUE(mutation) << mutation.error().message;
    EXPECT_FALSE(
        reservations.try_acquire(axk::app::PathAccess{{"workspace", "fixture.hds"}, axk::app::PathAccessMode::shared}));
    const auto closed = sessions.close(opened->image_id, "owner-a");
    ASSERT_FALSE(closed);
    EXPECT_EQ(closed.error().code, "entry_in_use");
    sessions.abort_mutation(opened->image_id, "owner-a", opened->revision);
    const auto inspected = sessions.inspect(opened->image_id, "owner-a");
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_EQ(inspected->revision, opened->revision);
    EXPECT_TRUE(
        reservations.try_acquire(axk::app::PathAccess{{"workspace", "fixture.hds"}, axk::app::PathAccessMode::shared}));
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
    ASSERT_EQ(preview->lanes.size(), 1U);
    EXPECT_EQ(preview->lanes.front().bins.size(), 32U);
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

TEST_F(ImageSessionTest, UsesTheSamplePlaybackWindowForPreviewAndAudition) {
    patch_sample_window(root_ / "fixture.hds", 32U, 32U, 40U, 8U);
    axk::app::ImageSessionManager sessions{*sandbox_, 2U, 64U};
    const auto opened = sessions.open({"workspace", "fixture.hds"}, "owner-a");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto sample_objects = sessions.objects(opened->image_id, "owner-a", 64U, std::nullopt, "SBNK");
    const auto wave_objects = sessions.objects(opened->image_id, "owner-a", 64U, std::nullopt, "SMPL");
    ASSERT_TRUE(sample_objects) << sample_objects.error().message;
    ASSERT_TRUE(wave_objects) << wave_objects.error().message;
    ASSERT_FALSE(sample_objects->items.empty());
    ASSERT_FALSE(wave_objects->items.empty());
    const auto &sample = sample_objects->items.front();
    const auto wave = std::ranges::find(wave_objects->items, sample.name, &axk::app::ImageObjectItem::name);
    ASSERT_NE(wave, wave_objects->items.end());

    const auto sample_preview = sessions.preview(opened->image_id, "owner-a", sample.id, 32U);
    ASSERT_TRUE(sample_preview) << sample_preview.error().message;
    EXPECT_EQ(sample_preview->frame_count, 32U);
    ASSERT_EQ(sample_preview->lanes.size(), 1U);
    EXPECT_EQ(sample_preview->lanes.front().role, "LEFT");
    EXPECT_EQ(sample_preview->lanes.front().source_object_id, wave->id);
    EXPECT_EQ(sample_preview->lanes.front().frame_count, 32U);
    EXPECT_EQ(sample_preview->lanes.front().bins.size(), 32U);

    const auto sample_audition = sessions.prepare_audition(opened->image_id, "owner-a", sample.id);
    const auto wave_audition = sessions.prepare_audition(opened->image_id, "owner-a", wave->id);
    ASSERT_TRUE(sample_audition) << sample_audition.error().message;
    ASSERT_TRUE(wave_audition) << wave_audition.error().message;
    EXPECT_EQ(sample_audition->frame_count, 32U);
    EXPECT_EQ(sample_audition->loop_mode, 1U);
    EXPECT_EQ(sample_audition->loop_start_frame, 8U);
    EXPECT_EQ(sample_audition->loop_length_frames, 8U);
    EXPECT_EQ(wave_audition->frame_count, 132U);

    const auto sample_pcm = sessions.audition_range(sample_audition->audition_id, "owner-a", 44U,
                                                    32U * sample_audition->sample_width_bytes);
    const auto wave_pcm =
        sessions.audition_range(wave_audition->audition_id, "owner-a",
                                44U + 32U * static_cast<std::uint64_t>(wave_audition->sample_width_bytes),
                                32U * wave_audition->sample_width_bytes);
    ASSERT_TRUE(sample_pcm) << sample_pcm.error().message;
    ASSERT_TRUE(wave_pcm) << wave_pcm.error().message;
    EXPECT_EQ(sample_pcm->bytes, wave_pcm->bytes);

    const auto wave_preview = sessions.preview(opened->image_id, "owner-a", wave->id, 32U);
    ASSERT_TRUE(wave_preview) << wave_preview.error().message;
    EXPECT_EQ(wave_preview->frame_count, 132U);
    ASSERT_EQ(wave_preview->lanes.size(), 1U);
    EXPECT_EQ(wave_preview->lanes.front().role, "MONO");
    EXPECT_EQ(wave_preview->lanes.front().frame_count, 132U);
}

TEST_F(ImageSessionTest, RejectsSamplePlaybackWindowsOutsideStoredWaveData) {
    patch_sample_window(root_ / "fixture.hds", 120U, 16U, 120U, 16U);
    axk::app::ImageSessionManager sessions{*sandbox_, 2U, 64U};
    const auto opened = sessions.open({"workspace", "fixture.hds"}, "owner-a");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto objects = sessions.objects(opened->image_id, "owner-a", 64U, std::nullopt, "SBNK");
    ASSERT_TRUE(objects) << objects.error().message;
    ASSERT_FALSE(objects->items.empty());
    const auto audition = sessions.prepare_audition(opened->image_id, "owner-a", objects->items.front().id);
    ASSERT_FALSE(audition);
    EXPECT_EQ(audition.error().code, "invalid_audio_range");
}

TEST_F(ImageSessionTest, UsesIndependentStereoMemberWindowsAndPadsTheShorterLane) {
    patch_sample_window(root_ / "fixture.hds", 32U, 32U, 40U, 8U, std::array{64U, 16U, 72U, 8U});
    axk::app::ImageSessionManager sessions{*sandbox_, 2U, 64U};
    const auto opened = sessions.open({"workspace", "fixture.hds"}, "owner-a");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto objects = sessions.objects(opened->image_id, "owner-a", 64U, std::nullopt, "SBNK");
    ASSERT_TRUE(objects) << objects.error().message;
    ASSERT_FALSE(objects->items.empty());

    const auto preview = sessions.preview(opened->image_id, "owner-a", objects->items.front().id, 32U);
    ASSERT_TRUE(preview) << preview.error().message;
    EXPECT_EQ(preview->frame_count, 32U);
    ASSERT_EQ(preview->lanes.size(), 2U);
    EXPECT_EQ(preview->lanes[0].role, "LEFT");
    EXPECT_EQ(preview->lanes[0].frame_count, 32U);
    EXPECT_EQ(preview->lanes[1].role, "RIGHT");
    EXPECT_EQ(preview->lanes[1].frame_count, 16U);

    const auto audition = sessions.prepare_audition(opened->image_id, "owner-a", objects->items.front().id);
    ASSERT_TRUE(audition) << audition.error().message;
    EXPECT_EQ(audition->channels, 2U);
    EXPECT_EQ(audition->frame_count, 32U);
    EXPECT_EQ(audition->loop_start_frame, 8U);
    EXPECT_EQ(audition->loop_length_frames, 8U);
    EXPECT_TRUE(audition->warnings.empty());
    const auto pcm = sessions.audition_range(audition->audition_id, "owner-a", 44U, 32U * 2U * 2U);
    ASSERT_TRUE(pcm) << pcm.error().message;
    ASSERT_EQ(pcm->bytes.size(), 128U);
    for (std::size_t frame = 16U; frame < 32U; ++frame) {
        EXPECT_EQ(pcm->bytes[frame * 4U + 2U], std::byte{0});
        EXPECT_EQ(pcm->bytes[frame * 4U + 3U], std::byte{0});
    }
}

TEST_F(ImageSessionTest, PreparesSampleAuditionWhenTheNamedWaveDataHasAStaleCachedReference) {
    patch_sample_cached_reference(root_ / "fixture.hds", 0xdeadbeefU);
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
