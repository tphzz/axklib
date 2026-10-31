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
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/application/image_sessions.hpp"
#include "axklib/application/jobs.hpp"
#include "axklib/formats/tx16w/document.hpp"
#include "axklib/media.hpp"
#include "axklib/native/registry.hpp"
#include "conversion_sources.hpp"
#include "image_sources.hpp"
#include "source_verification_test_access.hpp"

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

TEST_F(ImageSourcesTest, DirectoryDiscoveryUsesConstantWholeTreePassesAndLinearFileChecks) {
    std::vector<axk::app::SourceVerificationCounts> work;
    for (const auto count : {16U, 32U}) {
        const auto folder = "discovery-" + std::to_string(count);
        std::filesystem::create_directory(root / folder);
        for (std::size_t index = 0U; index < count; ++index)
            write_wave(folder + "/TONE" + std::to_string(index) + ".W01");
        const auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
        ASSERT_TRUE(sandbox);
        axk::app::SourceVerificationCounts counts;
        const axk::app::SourceVerificationObservation observe{counts};
        const std::vector<axk::app::ImageSourceRef> references{{"root", folder, axk::app::ImageSourceKind::directory}};
        const auto admitted = axk::app::admit_image_sources(*sandbox, nullptr, nullptr, references, "owner");
        ASSERT_TRUE(admitted);
        axk::native::FormatRegistry formats;
        ASSERT_TRUE(formats.add(axk::tx16w::provider()));
        const auto opened = formats.open(admitted->native_sources);
        ASSERT_TRUE(opened) << opened.error().message;
        ASSERT_EQ(opened->documents.size(), 1U);
        EXPECT_LE(counts.tree_opens, 8U);
        EXPECT_LE(counts.file_checks, 12U * count);
        work.push_back(counts);
    }
    EXPECT_EQ(work[0].tree_opens, work[1].tree_opens);
    EXPECT_LE(work[1].file_checks, 2U * work[0].file_checks + 8U);
}

TEST_F(ImageSourcesTest, SelectedReadsRejectReplacementWithoutScanningUnrelatedFiles) {
    auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
    ASSERT_TRUE(sandbox);
    const std::vector<axk::app::ImageSourceRef> references{{"root", "folder", axk::app::ImageSourceKind::directory}};
    const auto admitted = axk::app::admit_image_sources(*sandbox, nullptr, nullptr, references, "owner");
    ASSERT_TRUE(admitted);
    auto location = admitted->native_sources->locate("source-1", "directory", "three.bin");
    ASSERT_TRUE(location);
    location->length = 1U;
    axk::app::SourceVerificationCounts counts;
    const axk::app::SourceVerificationObservation observation{counts};
    EXPECT_TRUE(admitted->native_sources->read(*location));
    EXPECT_EQ(counts.tree_opens, 0U);
    EXPECT_EQ(counts.file_checks, 2U);
    const auto file = root / "folder/three.bin";
    std::filesystem::rename(file, root / "retained.bin");
    std::filesystem::copy_file(root / "retained.bin", file);
    EXPECT_FALSE(admitted->native_sources->read(*location));
}

TEST_F(ImageSourcesTest, DirectoryVerificationAndTraversalPropagateCancellation) {
    const auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
    ASSERT_TRUE(sandbox);
    const std::vector<axk::app::ImageSourceRef> references{{"root", "folder", axk::app::ImageSourceKind::directory}};
    const auto admitted = axk::app::admit_image_sources(*sandbox, nullptr, nullptr, references, "owner");
    ASSERT_TRUE(admitted);
    axk::CancellationSource cancellation;
    cancellation.cancel();
    const auto verified = admitted->native_sources->verify(cancellation.token());
    ASSERT_FALSE(verified);
    EXPECT_EQ(verified.error().code, axk::ErrorCode::operation_cancelled);
    const auto tree = sandbox->open_tree({"root", "folder"}, {32U, 1024U}, cancellation.token());
    ASSERT_FALSE(tree);
    EXPECT_EQ(tree.error().code, "operation_cancelled");
}

TEST_F(ImageSourcesTest, ActiveMemberVerificationStopsBetweenFileChecks) {
    write_wave("folder/TONE.W01");
    const auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
    ASSERT_TRUE(sandbox);
    const std::vector<axk::app::ImageSourceRef> references{{"root", "folder", axk::app::ImageSourceKind::directory}};
    const auto admitted = axk::app::admit_image_sources(*sandbox, nullptr, nullptr, references, "owner");
    ASSERT_TRUE(admitted);
    axk::CancellationSource cancellation;
    axk::app::SourceVerificationCounts counts;
    counts.on_file_check = [&] { cancellation.cancel(); };
    const axk::app::SourceVerificationObservation observe{counts};
    const auto verified = admitted->native_sources->verify(cancellation.token());
    ASSERT_FALSE(verified);
    EXPECT_EQ(verified.error().code, axk::ErrorCode::operation_cancelled);
    EXPECT_EQ(counts.file_checks, 1U);
}

TEST_F(ImageSourcesTest, FinalAdmissionVerificationKeepsCancellationDistinctFromSourceChanges) {
    const auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
    ASSERT_TRUE(sandbox);
    axk::CancellationSource cancellation;
    axk::app::SourceVerificationCounts counts;
    counts.on_tree_open = [&] {
        if (counts.tree_opens == 3U)
            cancellation.cancel();
    };
    const axk::app::SourceVerificationObservation observe{counts};
    const std::vector<axk::app::ImageSourceRef> references{{"root", "folder", axk::app::ImageSourceKind::directory}};
    const auto admitted =
        axk::app::admit_image_sources(*sandbox, nullptr, nullptr, references, "owner", cancellation.token());
    ASSERT_FALSE(admitted);
    EXPECT_EQ(admitted.error().code, "operation_cancelled");
    EXPECT_EQ(counts.tree_opens, 3U);
}

TEST_F(ImageSourcesTest, ActiveVerificationCancellationPublishesCancelledJobsForExplicitAndWorkspaceSources) {
    write_wave("folder/TONE.W01");
    const auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
    ASSERT_TRUE(sandbox);
    axk::app::ImageSessionManager sessions{*sandbox};
    const auto opened = sessions.open({{{"root", "folder", axk::app::ImageSourceKind::directory}}, {}}, "owner");
    ASSERT_TRUE(opened);
    axk::app::UploadStore uploads{root / "uploads", 1024U, 1024U, 1U, 1024U, std::chrono::minutes{1}};
    axk::native::FormatRegistry formats;
    ASSERT_TRUE(formats.add(axk::tx16w::provider()));
    for (const auto phase : {3U, 7U, 0U}) {
        axk::app::OperationRegistry registry;
        axk::app::JobManager *manager{};
        std::string job_id;
        bool cancelled{};
        ASSERT_TRUE(registry.declare({"test.verify",
                                      "Verify",
                                      axk::app::HttpMethod::post,
                                      "/api/v1/test/verify",
                                      axk::app::ExecutionMode::job,
                                      {},
                                      "Request",
                                      "Result"}));
        ASSERT_TRUE(registry.bind(
            "test.verify",
            [&](const auto &, const axk::app::OperationContext &context) -> axk::app::Result<nlohmann::json> {
                axk::app::SourceVerificationCounts counts;
                const auto cancel = [&] {
                    cancelled = true;
                    EXPECT_TRUE(manager->cancel(job_id, "owner"));
                };
                if (phase)
                    counts.on_tree_open = [&] {
                        if (counts.tree_opens == phase)
                            cancel();
                    };
                else
                    counts.on_file_check = [&] {
                        if (counts.file_checks == 1U)
                            cancel();
                    };
                const axk::app::SourceVerificationObservation observe{counts};
                nlohmann::json source = {
                    {"kind", "SOURCES"},
                    {"sources",
                     nlohmann::json::array(
                         {{{"kind", "DIRECTORY"}, {"directory", {{"rootId", "root"}, {"relativePath", "folder"}}}}})}};
                if (!phase)
                    source = {{"kind", "DOCUMENT"},
                              {"imageId", opened->image_id},
                              {"documentId", opened->documents.front().id},
                              {"expectedRevision", opened->revision}};
                const auto result = axk::app::conversion_internal::open_source(source, formats, "yamaha.tx16w",
                                                                               *sandbox, uploads, sessions, context);
                if (!result)
                    return std::unexpected(result.error());
                return nlohmann::json::object();
            }));
        axk::app::JobManager jobs{registry, 1U, 1U, 4U, 8U};
        manager = &jobs;
        const auto subscription = jobs.subscribe([&](const axk::app::JobEvent &event) {
            if (event.type == "queued")
                job_id = event.job_id;
        });
        axk::app::OperationContext context{};
        context.owner_id = "owner";
        const auto submitted = jobs.submit("test.verify", {}, context);
        ASSERT_TRUE(submitted);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            if (axk::app::is_terminal(jobs.status(submitted->job_id, "owner")->state))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        jobs.shutdown();
        EXPECT_TRUE(cancelled);
        EXPECT_EQ(jobs.status(submitted->job_id, "owner")->state, axk::app::JobState::cancelled);
        jobs.unsubscribe(subscription);
    }
}

TEST_F(ImageSourcesTest, PreviewTreeChecksDoNotGrowWithPcmWindowCount) {
    std::vector<axk::app::SourceVerificationCounts> work;
    for (const std::uint32_t frames : {16'384U, 65'536U}) {
        const auto folder = "audio-" + std::to_string(frames);
        std::filesystem::create_directory(root / folder);
        write_wave(folder + "/OTHER.W01");
        std::vector<char> bytes(32U + frames / 2U * 3U);
        std::ranges::copy(std::string_view{"LM8953"}, bytes.begin());
        bytes[22] = static_cast<char>(0xc9);
        bytes[23] = 2;
        bytes[24] = static_cast<char>(frames & 0xffU);
        bytes[25] = static_cast<char>((frames >> 8U) & 0xffU);
        bytes[26] = static_cast<char>((frames >> 16U) & 1U);
        {
            std::ofstream output{root / folder / "LONG.W01", std::ios::binary};
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        const auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
        ASSERT_TRUE(sandbox);
        axk::app::ImageSessionManager sessions{*sandbox};
        const auto opened = sessions.open({{{"root", folder, axk::app::ImageSourceKind::directory}}, {}}, "owner");
        ASSERT_TRUE(opened) << opened.error().message;
        const axk::app::ImageDocumentRef target{opened->image_id, opened->documents.front().id};
        const auto objects = sessions.objects(target, "owner", 100U);
        ASSERT_TRUE(objects);
        const auto wave = std::ranges::find(objects->items, "LONG.W01", &axk::app::ImageObjectItem::name);
        ASSERT_NE(wave, objects->items.end());
        axk::app::SourceVerificationCounts counts;
        const axk::app::SourceVerificationObservation observe{counts};
        const auto preview = sessions.preview(target, "owner", wave->id, 8U, opened->revision);
        ASSERT_TRUE(preview) << preview.error().message;
        ASSERT_EQ(preview->lanes.size(), 1U);
        EXPECT_EQ(preview->lanes.front().stored_frame_count, frames);
        EXPECT_LE(counts.tree_opens, 2U);
        work.push_back(counts);
    }
    EXPECT_EQ(work[0].tree_opens, work[1].tree_opens);
    EXPECT_LT(work[0].file_checks, work[1].file_checks);
}

TEST_F(ImageSourcesTest, AudioOperationsRecheckMembershipBeforePublishingResults) {
    for (const auto operation : {"preview", "prepare", "range"}) {
        const auto folder = std::string{operation};
        std::filesystem::create_directory(root / folder);
        write_wave(folder + "/TONE.W01");
        const auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
        ASSERT_TRUE(sandbox);
        axk::app::ImageSessionManager sessions{*sandbox};
        const auto opened = sessions.open({{{"root", folder, axk::app::ImageSourceKind::directory}}, {}}, "owner");
        ASSERT_TRUE(opened);
        const axk::app::ImageDocumentRef target{opened->image_id, opened->documents.front().id};
        const auto objects = sessions.objects(target, "owner", 10U);
        ASSERT_TRUE(objects);
        ASSERT_EQ(objects->items.size(), 1U);
        const auto &id = objects->items.front().id;
        const auto prepared = sessions.prepare_audition(target, "owner", {id}, opened->revision);
        ASSERT_TRUE(prepared);
        axk::app::SourceVerificationCounts counts;
        counts.on_tree_open = [&] {
            if (counts.tree_opens == 2U)
                write_wave(folder + "/NEW.W01");
        };
        const axk::app::SourceVerificationObservation observe{counts};
        const auto check = [](const auto &result) {
            EXPECT_FALSE(result);
            if (!result)
                EXPECT_EQ(result.error().code, "image_source_changed");
        };
        if (folder == "preview")
            check(sessions.preview(target, "owner", id, 8U, opened->revision));
        else if (folder == "prepare")
            check(sessions.prepare_audition(target, "owner", {id}, opened->revision));
        else
            check(sessions.audition_range(prepared->audition_id, "owner", 0U, 44U));
        EXPECT_EQ(counts.tree_opens, 2U);
    }
}

TEST_F(ImageSourcesTest, SelectedReadsRejectTruncationAndTreeTraversalRetainsQuotaFailures) {
    const auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
    ASSERT_TRUE(sandbox);
    const std::vector<axk::app::ImageSourceRef> references{{"root", "folder", axk::app::ImageSourceKind::directory}};
    const auto admitted = axk::app::admit_image_sources(*sandbox, nullptr, nullptr, references, "owner");
    ASSERT_TRUE(admitted);
    const auto location = admitted->native_sources->locate("source-1", "directory", "three.bin");
    ASSERT_TRUE(location);
    const auto quota = sandbox->open_tree({"root", "folder"}, {32U, 1U});
    ASSERT_FALSE(quota);
    EXPECT_EQ(quota.error().code, "download_archive_too_large");
    std::filesystem::resize_file(root / "folder/three.bin", 0U);
    EXPECT_FALSE(admitted->native_sources->read(*location));
}

TEST_F(ImageSourcesTest, NewSymlinksRemainVerificationFailures) {
    const auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
    ASSERT_TRUE(sandbox);
    const std::vector<axk::app::ImageSourceRef> references{{"root", "folder", axk::app::ImageSourceKind::directory}};
    const auto admitted = axk::app::admit_image_sources(*sandbox, nullptr, nullptr, references, "owner");
    ASSERT_TRUE(admitted);
    std::error_code error;
    std::filesystem::create_symlink(root / "one.bin", root / "folder/link.bin", error);
    if (error)
        GTEST_SKIP() << "Symlinks unavailable: " << error.message();
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
    const auto directory = root / "folder";
    const auto file = directory / "three.bin";
    const auto directory_time = std::filesystem::last_write_time(directory);
    const auto file_time = std::filesystem::last_write_time(file);
    const auto original = root / "original.bin";
    std::error_code error;
    std::filesystem::rename(file, original, error);
    ASSERT_FALSE(error) << error.message();
    ASSERT_TRUE(std::filesystem::copy_file(original, file));
    std::filesystem::last_write_time(file, file_time);
    std::filesystem::last_write_time(directory, directory_time);
    ASSERT_EQ(std::filesystem::file_size(file), std::filesystem::file_size(original));
    EXPECT_FALSE(sources.native_sources->verify());
}

TEST_F(ImageSourcesTest, RejectsDirectoryReplacementWithMatchingMembership) {
    // Windows cannot rename a parent while its child files are held open.
    const auto directory = root / "empty";
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    const auto directory_time = std::filesystem::last_write_time(directory);
    auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
    ASSERT_TRUE(sandbox);
    const std::vector<axk::app::ImageSourceRef> references{{"root", "empty", axk::app::ImageSourceKind::directory}};
    auto sources = axk::app::admit_image_sources(*sandbox, nullptr, nullptr, references, "owner");
    ASSERT_TRUE(sources) << sources.error().message;
    ASSERT_TRUE(sources->native_sources->verify());
    std::error_code error;
    std::filesystem::rename(directory, root / "original", error);
    ASSERT_FALSE(error) << error.message();
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    std::filesystem::last_write_time(directory, directory_time);
    EXPECT_FALSE(sources->native_sources->verify());
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
    ASSERT_EQ(opened->documents.size(), 3U);
    const axk::app::ImageDocumentRef a{opened->image_id, "document-a-series-source-1"};
    const axk::app::ImageDocumentRef tx{opened->image_id, "document-tx16w-source-2"};
    auto objects = sessions.objects(tx, "owner", 100U);
    ASSERT_TRUE(objects) << objects.error().message;
    ASSERT_EQ(objects->items.size(), 1U);
    const axk::app::ImageDocumentRef other{opened->image_id, "document-tx16w-source-3"};
    auto other_objects = sessions.objects(other, "owner", 100U);
    ASSERT_TRUE(other_objects);
    ASSERT_EQ(other_objects->items.size(), 1U);
    EXPECT_NE(objects->items.front().id, other_objects->items.front().id);
    EXPECT_FALSE(sessions.object_detail(tx, "owner", other_objects->items.front().id));
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
    const axk::app::ImageDocumentRef tx{opened->image_id, "document-tx16w-source-2"};
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
