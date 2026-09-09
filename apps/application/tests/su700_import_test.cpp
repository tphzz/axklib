#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "../../../library/tests/su700_test_fixture.hpp"
#include "axklib/application/su700_import_operations.hpp"
#include "axklib/su700.hpp"
#include "axklib/writer.hpp"
#include "content_digest.hpp"

namespace {
using Json = nlohmann::json;
class Su700Import : public testing::Test {
  protected:
    std::filesystem::path root;
    std::unique_ptr<axk::app::Sandbox> sandbox;
    std::unique_ptr<axk::app::ImageSessionManager> sessions;
    axk::app::PathReservationCoordinator reservations;
    axk::app::ImageSessionSummary image;
    std::string root_id;
    void SetUp() override {
        root = std::filesystem::temp_directory_path() /
               std::format("axk-su700-{}", std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(root / "workspace");
        ASSERT_TRUE(axk::write_hds_image({"1.0", 8U * 1024U * 1024U, {{"Disk", {}}}}, root / "workspace/image.hds"));
        auto created = axk::app::Sandbox::create({{"workspace", "Workspace", root / "workspace", true}});
        ASSERT_TRUE(created);
        sandbox = std::make_unique<axk::app::Sandbox>(std::move(*created));
        sessions = std::make_unique<axk::app::ImageSessionManager>(*sandbox, 32U, 500U, std::chrono::minutes{15},
                                                                   std::chrono::steady_clock::now, &reservations);
        const auto opened = sessions->open({"workspace", "image.hds"}, "owner");
        ASSERT_TRUE(opened);
        image = *opened;
        const auto bytes = su700_floppy_bytes();
        {
            std::ofstream output{root / "workspace/source.img", std::ios::binary};
            output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            ASSERT_TRUE(output);
        }
        const auto fat = axk::FatImage::open(std::make_shared<axk::MemoryReader>(bytes));
        ASSERT_TRUE(fat);
        const auto plan = axk::inspect_su700_floppy(*fat);
        ASSERT_TRUE(plan);
        std::vector<axk::FilesystemEdit> edits{axk::CreateFilesystemDirectory{{"SEED"}},
                                               axk::CreateFilesystemDirectory{{"SEED", "SUSQ"}},
                                               axk::CreateFilesystemDirectory{{"SEED", "SUSP"}}};
        for (const auto &file : plan->files) {
            const auto entry = std::ranges::find(fat->files(), file.source_path, &axk::FatFile::path);
            ASSERT_NE(entry, fat->files().end());
            auto data = fat->read_file(*entry);
            ASSERT_TRUE(data);
            auto path = file.destination_path;
            path.insert(path.begin(), "SEED");
            edits.emplace_back(
                axk::PutFilesystemFile{std::move(path), std::make_shared<axk::MemoryReader>(std::move(*data))});
        }
        axk::app::AlterationJournalStore journals{root / "seed-journals"};
        const auto seeded = axk::app::apply_filesystem_edits(*sessions, journals, image.image_id, "owner",
                                                             image.revision, axk::PartitionIndex{0}, edits);
        ASSERT_TRUE(seeded) << seeded.error().message;
        image = *seeded;
        const auto roots = sessions->filesystem(image.image_id, "owner", image.revision);
        ASSERT_TRUE(roots);
        ASSERT_EQ(roots->root_capabilities.size(), 1U);
        EXPECT_EQ(roots->root_capabilities.front().supported_imports, (std::vector<std::string>{"SU700_FLOPPY"}));
        root_id = roots->root_capabilities.front().root_id;
    }
    void TearDown() override {
        sessions.reset();
        sandbox.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::string digest() const { return axk::app::detail::file_sha256(root / "workspace/image.hds").value(); }
    Json request() const {
        return {{"source", {{"fileRef", {{"rootId", "workspace"}, {"relativePath", "source.img"}}}}},
                {"destination",
                 {{"imageId", image.image_id},
                  {"expectedRevision", image.revision},
                  {"rootEntryId", root_id},
                  {"volumeName", "IMPORTED"}}},
                {"includedExtras", Json::array()}};
    }
};

TEST_F(Su700Import, InspectsWithoutWritingThenImportsExactPathsAndSelectedExtras) {
    axk::app::UploadStore uploads{root / "uploads", 1048576U, 1048576U, 8U, 1024U, std::chrono::minutes{5}};
    axk::app::AlterationJournalStore journals{root / "journals"};
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_su700_import_operations(registry, *sandbox, uploads, *sessions, journals));
    const axk::app::OperationContext context{"owner", "inspect", {}, nullptr, {}};
    auto input = request();
    const auto before = digest();
    const auto plan = registry.invoke("images.su700.import.inspect", input, context);
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_EQ(plan->at("status"), "COMPLETE");
    EXPECT_EQ(plan->at("destinationReady"), true);
    EXPECT_EQ(digest(), before);
    input["expectedSource"] = plan->at("snapshot");
    auto stale = input;
    stale["expectedSource"]["sha256"] = std::string(64U, '0');
    EXPECT_FALSE(registry.invoke("images.su700.import", stale, context));
    EXPECT_EQ(digest(), before);
    const auto result = registry.invoke("images.su700.import", input, context);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->at("revision"), image.revision + 1U);
    const auto page =
        sessions->filesystem(image.image_id, "owner", image.revision + 1U, {.root_id = root_id, .query = "SONG"});
    ASSERT_TRUE(page);
    EXPECT_TRUE(std::ranges::any_of(page->items,
                                    [](const auto &entry) { return entry.path == "/IMPORTED/SUSQ/SONG    .SSQ"; }));
    const auto extras =
        sessions->filesystem(image.image_id, "owner", image.revision + 1U, {.root_id = root_id, .query = "README"});
    ASSERT_TRUE(extras);
    EXPECT_TRUE(
        std::ranges::none_of(extras->items, [](const auto &entry) { return entry.path == "/IMPORTED/README_L"; }));
    EXPECT_FALSE(registry.invoke("images.su700.import", input, context));
    input.erase("expectedSource");
    input["destination"]["expectedRevision"] = image.revision + 1U;
    const auto collision = registry.invoke("images.su700.import.inspect", input, context);
    ASSERT_TRUE(collision);
    EXPECT_EQ(collision->at("destinationReady"), false);
}

TEST_F(Su700Import, RejectsInvalidTargetsExtrasOwnershipAndCancellationWithoutWrites) {
    axk::app::UploadStore uploads{root / "uploads", 1048576U, 1048576U, 8U, 1024U, std::chrono::minutes{5}};
    axk::app::AlterationJournalStore journals{root / "journals"};
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_su700_import_operations(registry, *sandbox, uploads, *sessions, journals));
    const axk::app::OperationContext context{"owner", "inspect", {}, nullptr, {}};
    const auto before = digest();
    auto input = request();
    for (const auto &name : {"SEED", "../escape", "bad/child", "seventeen-letters!"}) {
        input["destination"]["volumeName"] = name;
        const auto result = registry.invoke("images.su700.import.inspect", input, context);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->at("destinationReady"), false);
    }
    input = request();
    input["includedExtras"] = {"SONG.SSQ"};
    EXPECT_FALSE(registry.invoke("images.su700.import.inspect", input, context));
    input = request();
    input["destination"]["rootEntryId"] = "missing";
    const auto missing = registry.invoke("images.su700.import.inspect", input, context);
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->at("destinationReady"), false);
    input = request();
    const auto foreign = registry.invoke("images.su700.import.inspect", input, {"other", "inspect", {}, nullptr, {}});
    ASSERT_TRUE(foreign);
    EXPECT_EQ(foreign->at("destinationReady"), false);
    axk::CancellationSource cancellation;
    cancellation.cancel();
    const auto cancelled = registry.invoke("images.su700.import.inspect", request(),
                                           {"owner", "cancel", cancellation.token(), nullptr, {}});
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, "operation_cancelled");
    EXPECT_EQ(digest(), before);
}

TEST_F(Su700Import, ReadOnlyTargetsDoNotAdvertiseOrPlanImports) {
    sessions.reset();
    auto readonly = axk::app::Sandbox::create({{"workspace", "Workspace", root / "workspace", false}});
    ASSERT_TRUE(readonly);
    sandbox = std::make_unique<axk::app::Sandbox>(std::move(*readonly));
    sessions = std::make_unique<axk::app::ImageSessionManager>(*sandbox, 32U, 500U, std::chrono::minutes{15},
                                                               std::chrono::steady_clock::now, &reservations);
    const auto opened = sessions->open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    image = *opened;
    const auto page = sessions->filesystem(image.image_id, "owner", image.revision);
    ASSERT_TRUE(page);
    EXPECT_TRUE(page->root_capabilities.front().supported_imports.empty());
    const std::vector<axk::FilesystemEdit> edits{axk::CreateFilesystemDirectory{{"NEW"}}};
    EXPECT_FALSE(sessions->inspect_su700_import(image.image_id, "owner", image.revision, root_id, "NEW", edits));
}

TEST_F(Su700Import, CapacityPlanningRejectsOversizedVolumesWithoutMutation) {
    const auto before = digest();
    const std::vector<axk::FilesystemEdit> edits{
        axk::CreateFilesystemDirectory{{"NEW"}},
        axk::PutFilesystemFile{{"NEW", "LARGE"},
                               std::make_shared<axk::MemoryReader>(std::vector<std::byte>(8U * 1024U * 1024U))}};
    EXPECT_FALSE(sessions->inspect_su700_import(image.image_id, "owner", image.revision, root_id, "NEW", edits));
    EXPECT_EQ(digest(), before);
}

TEST_F(Su700Import, RollsBackWhenSourceIdentityChangesDuringCommit) {
    axk::app::UploadStore uploads{root / "uploads", 1048576U, 1048576U, 8U, 1024U, std::chrono::minutes{5}};
    bool changed = false;
    axk::app::AlterationJournalStore journals{
        root / "journals", 32U * 1024U * 1024U, [&](std::string_view phase, std::size_t) {
            if (phase == "after-patch" && !changed) {
                std::fstream source{root / "workspace/source.img", std::ios::binary | std::ios::in | std::ios::out};
                source.seekp(100);
                source.put('X');
                changed = true;
            }
            return false;
        }};
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_su700_import_operations(registry, *sandbox, uploads, *sessions, journals));
    const axk::app::OperationContext context{"owner", "inspect", {}, nullptr, {}};
    auto input = request();
    const auto plan = registry.invoke("images.su700.import.inspect", input, context);
    ASSERT_TRUE(plan);
    input["expectedSource"] = plan->at("snapshot");
    const auto before = digest();
    const auto result = registry.invoke("images.su700.import", input, context);
    ASSERT_FALSE(result);
    EXPECT_TRUE(changed);
    EXPECT_EQ(result.error().code, "filesystem_input_changed");
    EXPECT_EQ(digest(), before);
    EXPECT_TRUE(sessions->begin_read(image.image_id, "owner", image.revision));
}
} // namespace
