#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/application/application_operations.hpp"
#include "axklib/audio.hpp"

namespace {

std::vector<std::byte> read_bytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    const std::vector<char> bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    return {reinterpret_cast<const std::byte *>(bytes.data()),
            reinterpret_cast<const std::byte *>(bytes.data() + bytes.size())};
}

class AudioOperationsTest : public testing::Test {
  protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() / "axklib-audio-operations-test";
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        std::filesystem::create_directories(root_ / "uploads");
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

    axk::app::Result<axk::app::UploadSnapshot> upload(std::string filename, axk::app::UploadKind kind) {
        axk::Waveform waveform;
        waveform.format = {1U, 2U, 96'000U};
        waveform.frame_count = 3U;
        waveform.pcm = {std::byte{}, std::byte{}, std::byte{0xe8}, std::byte{0x03}, std::byte{0x18}, std::byte{0xfc}};
        const auto path = root_ / filename;
        EXPECT_TRUE(axk::write_wav_atomic(path, waveform));
        const auto bytes = read_bytes(path);
        auto snapshot =
            uploads_->create({.owner_id = "owner",
                              .filename = filename,
                              .kind = kind,
                              .media_type = kind == axk::app::UploadKind::manifest ? "application/json" : "audio/wav",
                              .declared_size = bytes.size(),
                              .sha256 = std::nullopt});
        if (!snapshot)
            return std::unexpected(snapshot.error());
        snapshot = uploads_->append(snapshot->reference, "owner", 0U, bytes);
        if (!snapshot)
            return std::unexpected(snapshot.error());
        snapshot = uploads_->complete(snapshot->reference, "owner");
        return snapshot;
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

TEST_F(AudioOperationsTest, InspectsOwnedAudioUploadsWithSamplerConversionMetadata) {
    const auto snapshot = upload("source.wav", axk::app::UploadKind::audio);
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    const auto result = registry_.invoke(
        "audio.inspect", {{"source", {{"uploadRef", {{"uploadId", snapshot->reference.upload_id}}}}}}, context());
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->at("sourceFormat"), "WAV");
    EXPECT_EQ(result->at("channels"), 1U);
    EXPECT_EQ(result->at("frameCount"), 3U);
    EXPECT_EQ(result->at("sourceSampleRate"), 96'000U);
    EXPECT_EQ(result->at("outputSampleRate"), 44'100U);
    EXPECT_TRUE(result->at("resampled").get<bool>());
    EXPECT_EQ(result->at("projectedOutputFrameCount"), 2U);
    EXPECT_EQ(result->at("projectedOutputBytesPerChannel"), 4U);
    EXPECT_EQ(result->at("projectedOutputBytesTotal"), 4U);
    EXPECT_EQ(result->at("maximumOutputBytesPerChannel"), 32U * 1024U * 1024U);
    EXPECT_TRUE(result->at("valid").get<bool>());
    EXPECT_TRUE(result->at("issues").empty());
}

TEST_F(AudioOperationsTest, InspectsSandboxAudioFiles) {
    const auto snapshot = upload("source.wav", axk::app::UploadKind::audio);
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    const auto result = registry_.invoke(
        "audio.inspect", {{"source", {{"fileRef", {{"rootId", "workspace"}, {"relativePath", "source.wav"}}}}}},
        context());
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->at("sourceFormat"), "WAV");
    EXPECT_EQ(result->at("channels"), 1U);
    EXPECT_EQ(result->at("frameCount"), 3U);
}

TEST_F(AudioOperationsTest, RejectsWrongUploadKindAndOwner) {
    const auto snapshot = upload("source.json", axk::app::UploadKind::manifest);
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    const auto input = nlohmann::json{{"source", {{"uploadRef", {{"uploadId", snapshot->reference.upload_id}}}}}};
    const auto wrong_kind = registry_.invoke("audio.inspect", input, context());
    ASSERT_FALSE(wrong_kind);
    EXPECT_EQ(wrong_kind.error().code, "upload_kind_mismatch");

    const auto wrong_owner = registry_.invoke("audio.inspect", input, context("other"));
    ASSERT_FALSE(wrong_owner);
    EXPECT_EQ(wrong_owner.error().code, "upload_not_found");
}

} // namespace
