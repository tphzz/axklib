#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/application/application_operations.hpp"

namespace {

std::vector<std::byte> midi_with_controllers_and_system_exclusive() {
    return {
        std::byte{'M'}, std::byte{'T'},  std::byte{'h'},  std::byte{'d'},  std::byte{},    std::byte{},
        std::byte{},    std::byte{6},    std::byte{},     std::byte{},     std::byte{},    std::byte{1},
        std::byte{},    std::byte{96},   std::byte{'M'},  std::byte{'T'},  std::byte{'r'}, std::byte{'k'},
        std::byte{},    std::byte{},     std::byte{},     std::byte{30},   std::byte{},    std::byte{0xb0},
        std::byte{71},  std::byte{64},   std::byte{},     std::byte{0xb0}, std::byte{74},  std::byte{32},
        std::byte{},    std::byte{0xf0}, std::byte{3},    std::byte{0x7d}, std::byte{1},   std::byte{0xf7},
        std::byte{},    std::byte{0xf7}, std::byte{1},    std::byte{2},    std::byte{96},  std::byte{0x90},
        std::byte{60},  std::byte{64},   std::byte{96},   std::byte{0x80}, std::byte{60},  std::byte{},
        std::byte{},    std::byte{0xff}, std::byte{0x2f}, std::byte{},
    };
}

class MidiOperationsTest : public testing::Test {
  protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() / "axklib-midi-operations-test";
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        std::filesystem::create_directories(root_);
        auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root_, true}});
        ASSERT_TRUE(sandbox);
        sandbox_ = std::make_unique<axk::app::Sandbox>(std::move(*sandbox));
        uploads_ = std::make_unique<axk::app::UploadStore>(root_ / "uploads", 16U * 1024U * 1024U, 8U * 1024U * 1024U,
                                                           8U, 2U * 1024U * 1024U, std::chrono::minutes{5});
        auto registry = axk::app::make_application_registry(*sandbox_, *uploads_);
        ASSERT_TRUE(registry) << registry.error().message;
        registry_ = std::move(*registry);
    }

    void TearDown() override {
        uploads_.reset();
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    axk::app::Result<axk::app::UploadSnapshot> upload(axk::app::UploadKind kind) {
        auto bytes = midi_with_controllers_and_system_exclusive();
        auto filename = std::string{"events.mid"};
        auto media_type = std::string{"audio/midi"};
        if (kind == axk::app::UploadKind::manifest) {
            bytes = {std::byte{'{'}, std::byte{'}'}};
            filename = "manifest.json";
            media_type = "application/json";
        }
        auto snapshot = uploads_->create({.owner_id = "owner",
                                          .filename = std::move(filename),
                                          .kind = kind,
                                          .media_type = std::move(media_type),
                                          .declared_size = bytes.size(),
                                          .sha256 = std::nullopt});
        if (!snapshot)
            return std::unexpected(snapshot.error());
        snapshot = uploads_->append(snapshot->reference, "owner", 0U, bytes);
        if (!snapshot)
            return std::unexpected(snapshot.error());
        return uploads_->complete(snapshot->reference, "owner");
    }

    axk::app::OperationContext context(std::string owner = "owner") const {
        return {.owner_id = std::move(owner),
                .request_id = "request",
                .cancellation = {},
                .progress = nullptr,
                .display_path = {}};
    }

    std::filesystem::path root_;
    std::unique_ptr<axk::app::Sandbox> sandbox_;
    std::unique_ptr<axk::app::UploadStore> uploads_;
    axk::app::OperationRegistry registry_;
};

TEST_F(MidiOperationsTest, InspectsControllersAndOpaqueSystemExclusiveMetadata) {
    const auto snapshot = upload(axk::app::UploadKind::midi);
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    const auto result = registry_.invoke(
        "midi.inspect", {{"source", {{"uploadRef", {{"uploadId", snapshot->reference.upload_id}}}}}}, context());
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->at("format"), 0U);
    EXPECT_EQ(result->at("trackCount"), 1U);
    EXPECT_EQ(result->at("ticksPerQuarterNote"), 96U);
    EXPECT_EQ(result->at("endTick"), 192U);
    EXPECT_EQ(result->at("eventCount"), 7U);
    EXPECT_EQ(result->at("channelEventCount"), 4U);
    EXPECT_EQ(result->at("metaEventCount"), 1U);
    EXPECT_EQ(result->at("systemExclusiveEventCount"), 2U);
    EXPECT_EQ(result->at("systemExclusiveDataBytes"), 4U);
    EXPECT_EQ(result->at("controllers"), nlohmann::json::array({{{"controller", 71U}, {"eventCount", 1U}},
                                                                {{"controller", 74U}, {"eventCount", 1U}}}));
    EXPECT_EQ(result->at("systemExclusiveManufacturerIds"), nlohmann::json::array({"7D"}));
    EXPECT_TRUE(result->at("systemExclusivePreservationSupported").get<bool>());
}

TEST_F(MidiOperationsTest, InspectsSandboxFilesAndRejectsWrongUploadKind) {
    const auto bytes = midi_with_controllers_and_system_exclusive();
    {
        std::ofstream output{root_ / "events.mid", std::ios::binary};
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const auto file = registry_.invoke(
        "midi.inspect", {{"source", {{"fileRef", {{"rootId", "workspace"}, {"relativePath", "events.mid"}}}}}},
        context());
    ASSERT_TRUE(file) << file.error().message;
    EXPECT_EQ(file->at("systemExclusiveEventCount"), 2U);

    const auto wrong_kind = upload(axk::app::UploadKind::manifest);
    ASSERT_TRUE(wrong_kind) << wrong_kind.error().message;
    const auto rejected = registry_.invoke(
        "midi.inspect", {{"source", {{"uploadRef", {{"uploadId", wrong_kind->reference.upload_id}}}}}}, context());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, "upload_kind_mismatch");
}

} // namespace
