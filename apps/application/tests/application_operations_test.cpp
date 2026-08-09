#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "axklib/application/application_operations.hpp"

namespace {

bool session_operation(std::string_view id) {
    return id == "auditions.prepare" || id == "files.archive" || id == "images.alter" ||
           id == "images.deletion.inspect" || id == "images.deletion.orphans.inspect" || id == "images.delete" ||
           id == "images.programs.generate.inspect" || id == "images.programs.generate" ||
           id == "images.volume_deletion.inspect" || id == "images.placement.inspect" ||
           id == "images.placement.repair" || id == "images.package_import.plan" ||
           id == "images.package_import.release" || id == "images.package_import" || id == "images.package_export" ||
           id == "images.audio_export.inspect" || id == "images.audio_export" ||
           id == "images.volume_package_export.inspect" || id == "images.volume_package_export" ||
           id == "images.volume_floppy_export.inspect" || id == "images.volume_floppy_export" ||
           id == "images.sequence_export" || id == "images.media_conversion.inspect" || id == "images.media_conversion";
}

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path() / ("axklib-application-operations-" + suffix);
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

TEST(ApplicationOperations, ComposesStatelessOperationsAndLeavesInteractiveSessionsToTheServer) {
    TemporaryDirectory temporary;
    const auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", temporary.path(), true}});
    ASSERT_TRUE(sandbox) << sandbox.error().message;
    axk::app::UploadStore uploads{temporary.path() / "uploads", 1024U, 1024U, 4U, 256U, std::chrono::seconds{30}};
    const auto registry = axk::app::make_application_registry(*sandbox, uploads);
    ASSERT_TRUE(registry) << registry.error().message;
    for (const auto &entry : registry->entries()) {
        if (session_operation(entry.descriptor.id)) {
            EXPECT_FALSE(entry.implemented);
            continue;
        }
        EXPECT_TRUE(entry.implemented) << entry.descriptor.id;
    }
}

TEST(ApplicationOperations, ProducesTheSameStatelessCallableInventoryForEveryTransportRegistry) {
    TemporaryDirectory temporary;
    const auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", temporary.path(), true}});
    ASSERT_TRUE(sandbox) << sandbox.error().message;
    axk::app::UploadStore first_uploads{
        temporary.path() / "first-uploads", 1024U, 1024U, 4U, 256U, std::chrono::seconds{30}};
    axk::app::UploadStore second_uploads{
        temporary.path() / "second-uploads", 1024U, 1024U, 4U, 256U, std::chrono::seconds{30}};

    const auto first = axk::app::make_application_registry(*sandbox, first_uploads);
    ASSERT_TRUE(first) << first.error().message;
    const auto second = axk::app::make_application_registry(*sandbox, second_uploads);
    ASSERT_TRUE(second) << second.error().message;

    const auto first_entries = first->entries();
    const auto second_entries = second->entries();
    ASSERT_EQ(first_entries.size(), second_entries.size());
    for (std::size_t index = 0; index < first_entries.size(); ++index) {
        EXPECT_EQ(first_entries[index].descriptor.id, second_entries[index].descriptor.id);
        const auto interactive_session_operation = session_operation(first_entries[index].descriptor.id);
        EXPECT_EQ(first_entries[index].implemented, !interactive_session_operation)
            << first_entries[index].descriptor.id;
        EXPECT_EQ(second_entries[index].implemented, !interactive_session_operation)
            << second_entries[index].descriptor.id;
        EXPECT_EQ(first_entries[index].handler_type_hash != 0U, !interactive_session_operation)
            << first_entries[index].descriptor.id;
        EXPECT_EQ(first_entries[index].handler_type_hash, second_entries[index].handler_type_hash)
            << first_entries[index].descriptor.id;
    }
}

TEST(ApplicationOperations, ValidationReservesExportsAsInputAndDestinationAsOutput) {
    TemporaryDirectory temporary;
    const auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", temporary.path(), true}});
    ASSERT_TRUE(sandbox) << sandbox.error().message;
    axk::app::UploadStore uploads{temporary.path() / "uploads", 1024U, 1024U, 4U, 256U, std::chrono::seconds{30}};
    const auto registry = axk::app::make_application_registry(*sandbox, uploads);
    ASSERT_TRUE(registry) << registry.error().message;

    const auto accesses =
        registry->path_accesses("report.validate",
                                {{"exports", {{"rootId", "workspace"}, {"relativePath", "exports"}}},
                                 {"destination", {{"rootId", "workspace"}, {"relativePath", "reports"}}}},
                                {});
    ASSERT_TRUE(accesses) << accesses.error().message;
    ASSERT_EQ(accesses->size(), 2U);
    EXPECT_EQ((*accesses)[0].reference.root_id, "workspace");
    EXPECT_EQ((*accesses)[0].reference.relative_path, "exports");
    EXPECT_EQ((*accesses)[0].mode, axk::app::PathAccessMode::shared);
    EXPECT_EQ((*accesses)[1].reference.root_id, "workspace");
    EXPECT_EQ((*accesses)[1].reference.relative_path, "reports");
    EXPECT_EQ((*accesses)[1].mode, axk::app::PathAccessMode::exclusive);
}

} // namespace
