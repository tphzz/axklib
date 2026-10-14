#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/application/image_sessions.hpp"
#include "axklib/media.hpp"
#include "image_sources.hpp"

namespace {

class ImageSourcesTest : public testing::Test {
  protected:
    std::filesystem::path root = std::filesystem::temp_directory_path() / "axklib-source-set-test";
    void SetUp() override {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "folder");
        std::ofstream(root / "one.bin", std::ios::binary) << "same bytes";
        std::ofstream(root / "two.bin", std::ios::binary) << "same bytes";
        std::ofstream(root / "folder" / "three.bin", std::ios::binary) << "third file";
    }
    void TearDown() override { std::filesystem::remove_all(root); }
    void write_wave(std::string_view name) {
        std::vector<char> bytes(35U);
        std::ranges::copy(std::string_view{"LM8953"}, bytes.begin());
        bytes[22] = static_cast<char>(0xc9);
        bytes[23] = bytes[24] = 2;
        bytes[32] = 0x7f;
        bytes[33] = static_cast<char>(0xf0);
        bytes[34] = static_cast<char>(0x80);
        std::ofstream output{root / name, std::ios::binary};
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(output);
    }
    void copy_a_series() {
        std::filesystem::copy_file(std::filesystem::path{AXK_SOURCE_ROOT} /
                                       "tests/fixtures/images/sampler-authored/HD00_512_single_sbnk_authored.hds",
                                   root / "a.hds");
    }
};

TEST_F(ImageSourcesTest, EqualBytesAreDistinctOccurrencesAndUnknownFilesRemainAvailable) {
    auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
    ASSERT_TRUE(sandbox);
    const std::vector<axk::app::ImageSourceRef> sources{{"root", "one.bin"}, {"root", "two.bin"}};
    auto admitted = axk::app::admit_image_sources(*sandbox, nullptr, nullptr, sources, "owner");
    ASSERT_TRUE(admitted) << admitted.error().message;
    ASSERT_EQ(admitted->members.size(), 2U);
    EXPECT_NE(admitted->members[0]->id, admitted->members[1]->id);
    EXPECT_FALSE(admitted->members[0]->media);
    EXPECT_EQ(admitted->members[0]->roots.front().entries.size(), 1U);
    EXPECT_TRUE(admitted->native_sources->verify());
}

TEST_F(ImageSourcesTest, RejectsEmptyOversizedDuplicateAndOverlappingSets) {
    auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
    ASSERT_TRUE(sandbox);
    std::vector<axk::app::ImageSourceRef> sources;
    EXPECT_FALSE(axk::app::admit_image_sources(*sandbox, nullptr, nullptr, sources, "owner"));
    sources.resize(33U, {"root", "one.bin"});
    EXPECT_FALSE(axk::app::admit_image_sources(*sandbox, nullptr, nullptr, sources, "owner"));
    sources.resize(2U);
    auto duplicate = axk::app::admit_image_sources(*sandbox, nullptr, nullptr, sources, "owner");
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, "duplicate_image_source");
    sources = {{"root", "folder", axk::app::ImageSourceKind::directory}, {"root", "folder/three.bin"}};
    auto overlap = axk::app::admit_image_sources(*sandbox, nullptr, nullptr, sources, "owner");
    ASSERT_FALSE(overlap);
    EXPECT_EQ(overlap.error().code, "overlapping_image_sources");
}

TEST_F(ImageSourcesTest, RetainsDirectoryMembershipAndFileIdentity) {
    auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
    ASSERT_TRUE(sandbox);
    const std::vector<axk::app::ImageSourceRef> sources{{"root", "folder", axk::app::ImageSourceKind::directory}};
    auto admitted = axk::app::admit_image_sources(*sandbox, nullptr, nullptr, sources, "owner");
    ASSERT_TRUE(admitted) << admitted.error().message;
    EXPECT_TRUE(admitted->native_sources->verify());
    std::ofstream(root / "folder" / "added.bin") << "new";
    EXPECT_FALSE(admitted->native_sources->verify());
}

TEST_F(ImageSourcesTest, RejectsDeviceMixingBeforeOpeningAnyDevice) {
    auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
    ASSERT_TRUE(sandbox);
    const std::vector<axk::app::ImageSourceRef> sources{{"root", "one.bin"},
                                                        {{}, {}, axk::app::ImageSourceKind::device, "owned", "target"}};
    auto admitted = axk::app::admit_image_sources(*sandbox, nullptr, nullptr, sources, "owner");
    ASSERT_FALSE(admitted);
    EXPECT_EQ(admitted.error().code, "invalid_image_sources");
}

TEST_F(ImageSourcesTest, RejectsSameSizedReplacementAndRetainsSandboxLifetime) {
    axk::app::AdmittedImageSources sources;
    {
        auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
        ASSERT_TRUE(sandbox);
        const std::vector<axk::app::ImageSourceRef> references{
            {"root", "folder", axk::app::ImageSourceKind::directory}};
        auto opened = axk::app::admit_image_sources(*sandbox, nullptr, nullptr, references, "owner");
        ASSERT_TRUE(opened);
        sources = std::move(*opened);
    }
    ASSERT_TRUE(sources.native_sources->verify());
    std::filesystem::rename(root / "folder", root / "original");
    std::filesystem::create_directory(root / "folder");
    std::ofstream(root / "folder" / "three.bin") << "third file";
    EXPECT_FALSE(sources.native_sources->verify());
}

TEST_F(ImageSourcesTest, RejectsHardLinkAliasesInsideSelectedDirectories) {
    std::error_code error;
    std::filesystem::create_hard_link(root / "folder" / "three.bin", root / "alias.bin", error);
    if (error)
        GTEST_SKIP() << "Hard links unavailable: " << error.message();
    auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
    ASSERT_TRUE(sandbox);
    const std::vector<axk::app::ImageSourceRef> references{{"root", "folder", axk::app::ImageSourceKind::directory},
                                                           {"root", "alias.bin"}};
    auto admitted = axk::app::admit_image_sources(*sandbox, nullptr, nullptr, references, "owner");
    ASSERT_FALSE(admitted);
    EXPECT_EQ(admitted.error().code, "overlapping_image_sources");
}

TEST_F(ImageSourcesTest, NativeSourcesRetainTheirAdmissionReservations) {
    auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
    ASSERT_TRUE(sandbox);
    axk::app::PathReservationCoordinator reservations;
    std::shared_ptr<const axk::native::SourceSet> retained;
    const axk::app::PathAccess write{{"root", "one.bin"}, axk::app::PathAccessMode::exclusive};
    {
        const std::vector<axk::app::ImageSourceRef> references{{"root", "one.bin"}};
        auto admitted = axk::app::admit_image_sources(*sandbox, &reservations, nullptr, references, "owner");
        ASSERT_TRUE(admitted);
        retained = admitted->native_sources;
    }
    EXPECT_FALSE(reservations.try_acquire(write));
    retained.reset();
    EXPECT_TRUE(reservations.try_acquire(write));
}

TEST_F(ImageSourcesTest, NativeDocumentsIsolateObjectsAndBindEveryOccurrenceToFiles) {
    copy_a_series();
    write_wave("TONE.W01");
    write_wave("OTHER.W01");
    auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
    ASSERT_TRUE(sandbox);
    axk::app::ImageSessionManager sessions{*sandbox};
    auto opened = sessions.open({{{"root", "a.hds"}, {"root", "TONE.W01"}, {"root", "OTHER.W01"}}, {}}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    ASSERT_EQ(opened->sources.size(), 3U);
    ASSERT_EQ(opened->documents.size(), 2U);
    const axk::app::ImageDocumentRef a{opened->image_id, "document-a-series-source-1"};
    const axk::app::ImageDocumentRef tx{opened->image_id, "document-tx16w"};
    auto objects = sessions.objects(tx, "owner", 100U);
    ASSERT_TRUE(objects) << objects.error().message;
    ASSERT_EQ(objects->items.size(), 2U);
    EXPECT_NE(objects->items[0].id, objects->items[1].id);
    for (const auto &object : objects->items) {
        auto detail = sessions.object_detail(tx, "owner", object.id);
        ASSERT_TRUE(detail) << detail.error().message;
        EXPECT_EQ((*detail)["documentId"], tx.document_id);
        EXPECT_EQ((*detail)["formatId"], "yamaha.tx16w");
        EXPECT_FALSE(sessions.object_detail(a, "owner", object.id));
        auto files =
            sessions.filesystem(opened->image_id, "owner", 1U, {.object_id = object.id, .document_id = tx.document_id});
        ASSERT_TRUE(files) << files.error().message;
        ASSERT_EQ(files->items.size(), 1U);
        EXPECT_NE(files->items.front().source_member_id, "source-1");
        ASSERT_EQ(files->items.front().bindings.size(), 1U);
        EXPECT_EQ(files->items.front().bindings.front().document_id, tx.document_id);
    }
    EXPECT_FALSE(sessions.objects(tx, "another-owner", 100U));
    EXPECT_FALSE(sessions.begin_mutation(tx, "owner", 1U));
    EXPECT_FALSE(sessions.filesystem(opened->image_id, "owner", 2U));
}

TEST_F(ImageSourcesTest, StandaloneASeriesObjectsExposeDocumentScopedFileBindings) {
    copy_a_series();
    {
        auto media = axk::open_media(root / "a.hds");
        ASSERT_TRUE(media);
        auto objects = media->objects();
        ASSERT_TRUE(objects);
        ASSERT_FALSE(objects->empty());
        const auto &bytes = objects->front().raw_payload;
        std::ofstream output{root / "object.bin", std::ios::binary};
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(output);
    }
    auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
    ASSERT_TRUE(sandbox);
    axk::app::ImageSessionManager sessions{*sandbox};
    auto opened = sessions.open({{{"root", "object.bin"}}, {}}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    ASSERT_EQ(opened->documents.size(), 1U);
    const axk::app::ImageDocumentRef target{opened->image_id, opened->documents.front().id};
    auto objects = sessions.objects(target, "owner", 100U);
    ASSERT_TRUE(objects);
    ASSERT_EQ(objects->items.size(), 1U);
    auto files = sessions.filesystem(opened->image_id, "owner", 1U,
                                     {.object_id = objects->items.front().id, .document_id = target.document_id});
    ASSERT_TRUE(files) << files.error().message;
    ASSERT_EQ(files->items.size(), 1U);
    EXPECT_EQ(files->items.front().id, "source-1:entry:file");
    EXPECT_EQ(files->items.front().bindings.front().object_id, objects->items.front().id);
}

TEST_F(ImageSourcesTest, ASeriesCommitPreservesOtherDocumentsAndRejectsUntouchedSourceChanges) {
    copy_a_series();
    write_wave("TONE.W01");
    auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
    ASSERT_TRUE(sandbox);
    axk::app::PathReservationCoordinator reservations;
    axk::app::ImageSessionManager sessions{
        *sandbox, 4U, 256U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    auto opened = sessions.open({{{"root", "a.hds"}, {"root", "TONE.W01"}}, {}}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const axk::app::ImageDocumentRef a{opened->image_id, "document-a-series-source-1"};
    const axk::app::ImageDocumentRef tx{opened->image_id, "document-tx16w"};
    auto before = sessions.objects(tx, "owner", 100U);
    ASSERT_TRUE(before);
    auto mutation = sessions.begin_mutation(a, "owner", 1U);
    ASSERT_TRUE(mutation) << mutation.error().message;
    auto refreshed = sessions.commit_mutation(opened->image_id, "owner", 1U);
    ASSERT_TRUE(refreshed) << refreshed.error().message;
    mutation->target.reset();
    ASSERT_EQ(refreshed->documents.size(), 2U);
    auto after = sessions.objects(tx, "owner", 100U);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->items.front().id, before->items.front().id);
    EXPECT_FALSE(sessions.begin_read(tx, "owner", 1U));
    mutation = sessions.begin_mutation(a, "owner", 2U);
    ASSERT_TRUE(mutation);
    std::ofstream(root / "TONE.W01", std::ios::binary) << "replaced";
    auto rejected = sessions.prepare_mutation_commit(opened->image_id, "owner", 2U);
    EXPECT_FALSE(rejected);
    sessions.abort_mutation(opened->image_id, "owner", 2U, true);
    mutation->target.reset();
}

} // namespace
