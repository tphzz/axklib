#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "../../../library/tests/su700_test_fixture.hpp"
#include "filesystem_image_inputs.hpp"
#include <gtest/gtest.h>

namespace {
class FilesystemImageInputs : public testing::Test {
  protected:
    std::filesystem::path root;
    std::unique_ptr<axk::app::Sandbox> sandbox;
    std::unique_ptr<axk::app::UploadStore> uploads;
    std::unique_ptr<axk::app::FilesystemImageInputs> inputs;
    axk::app::OperationContext context{
        .owner_id = "owner", .request_id = "inspect", .cancellation = {}, .progress = nullptr, .display_path = {}};
    const nlohmann::json source{{"fileRef", {{"rootId", "source"}, {"relativePath", "disk.ima"}}}};
    void write(const std::vector<std::byte> &bytes) {
        std::ofstream output{root / "disk.ima", std::ios::binary | std::ios::trunc};
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(output);
    }
    void SetUp() override {
        root = std::filesystem::temp_directory_path() /
               std::format("axk-image-inputs-{}", std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(root);
        auto created = axk::app::Sandbox::create({{"source", "Source", root, false}});
        ASSERT_TRUE(created);
        sandbox = std::make_unique<axk::app::Sandbox>(std::move(*created));
        uploads = std::make_unique<axk::app::UploadStore>(root / "uploads", 8388608U, 8388608U, 8U, 1024U,
                                                          std::chrono::minutes{5});
        inputs = std::make_unique<axk::app::FilesystemImageInputs>(*sandbox, *uploads);
        write(su700_floppy_bytes());
    }
    void TearDown() override {
        inputs.reset();
        uploads.reset();
        sandbox.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

TEST_F(FilesystemImageInputs, RetainsRawAuxiliaryFilesAndOwnerBoundLeasesAcrossRelease) {
    const auto result = inputs->inspect(source, context);
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ(result->at("entries").size(), 4U);
    const auto token = result->at("inspectionToken").get<std::string>();
    EXPECT_FALSE(inputs->lease(token, "other"));
    ASSERT_TRUE(inputs->release(token, "other"));
    const auto lease = inputs->lease(token, "owner");
    ASSERT_TRUE(lease);
    EXPECT_TRUE(lease->verify({}));
    EXPECT_FALSE(lease->open("missing"));
    for (const auto &entry : result->at("entries")) {
        const auto file = lease->open(entry.at("entryId").get<std::string>());
        ASSERT_TRUE(file);
        EXPECT_TRUE(file->verify(entry.at("snapshot")));
    }
    EXPECT_EQ(result->at("entries")[0].at("relativePath"), nlohmann::json::array({"README_L"}));
    EXPECT_TRUE(inputs->release(token, "owner"));
    EXPECT_FALSE(inputs->lease(token, "owner"));
    EXPECT_TRUE(lease->verify({}));
    EXPECT_TRUE(lease->open(result->at("entries")[0].at("entryId").get<std::string>()));
}

TEST_F(FilesystemImageInputs, RejectsChangedSourcesMalformedImagesBoundsAndCancellation) {
    const auto inspected = inputs->inspect(source, context);
    ASSERT_TRUE(inspected);
    const auto lease = inputs->lease(inspected->at("inspectionToken").get<std::string>(), "owner");
    ASSERT_TRUE(lease);
    const auto path = root / "disk.ima";
    std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds{1});
    EXPECT_FALSE(lease->verify({}));
    axk::CancellationSource cancellation;
    cancellation.cancel();
    context.cancellation = cancellation.token();
    EXPECT_FALSE(inputs->inspect(source, context));
    context.cancellation = {};
    write(std::vector<std::byte>(4194305U));
    EXPECT_FALSE(inputs->inspect(source, context));
    write(std::vector<std::byte>(512U));
    EXPECT_FALSE(inputs->inspect(source, context));
    write(su700_floppy_bytes());
    EXPECT_TRUE(inputs->inspect(source, context));
}

TEST_F(FilesystemImageInputs, BoundsRetainedInspectionsIncludingReleasedActiveLeases) {
    std::vector<axk::app::FilesystemImageLease> held;
    for (std::size_t i = 0; i < 32U; ++i) {
        const auto result = inputs->inspect(source, context);
        ASSERT_TRUE(result);
        const auto token = result->at("inspectionToken").get<std::string>();
        held.push_back(inputs->lease(token, "owner").value());
        EXPECT_TRUE(inputs->release(token, "owner"));
    }
    EXPECT_FALSE(inputs->inspect(source, context));
    held.clear();
    EXPECT_TRUE(inputs->inspect(source, context));
}

TEST_F(FilesystemImageInputs, ExpiresUnclaimedHandlesWithoutInvalidatingActiveReaders) {
    auto now = std::chrono::steady_clock::now();
    axk::app::FilesystemImageInputs expiring{*sandbox, *uploads, [&] { return now; }};
    const auto inspection = expiring.inspect(source, context);
    ASSERT_TRUE(inspection);
    const auto token = inspection->at("inspectionToken").get<std::string>();
    const auto lease = expiring.lease(token, "owner");
    ASSERT_TRUE(lease);
    now += std::chrono::minutes{16};
    EXPECT_FALSE(expiring.lease(token, "owner"));
    EXPECT_TRUE(lease->verify({}));
}
} // namespace
