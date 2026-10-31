#include "conversion_sources.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../../../library/tests/media_test_fixtures.hpp"
#include "axklib/formats/tx16w/document.hpp"

namespace {
using namespace axk::app;

class ConversionUploadVerification : public testing::TestWithParam<int> {
  protected:
    std::filesystem::path root = std::filesystem::temp_directory_path() / "axk-conversion-upload-verification";
    std::optional<Sandbox> sandbox;
    std::unique_ptr<UploadStore> uploads;
    std::unique_ptr<ImageSessionManager> images;
    axk::native::FormatRegistry formats;
    OperationContext context{};
    UploadRef reference;
    std::vector<std::byte> bytes;

    void SetUp() override {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        auto storage = Sandbox::create({{"root", "Root", root, false}});
        ASSERT_TRUE(storage);
        sandbox = std::move(*storage);
        uploads = std::make_unique<UploadStore>(root / "uploads", 1024U * 1024U, 1024U * 1024U, 4U, 1024U * 1024U,
                                                std::chrono::minutes{1});
        images = std::make_unique<ImageSessionManager>(*sandbox);
        ASSERT_TRUE(formats.add(axk::tx16w::provider()));
        context.owner_id = "owner";
        bytes = fat_fixture();
        ascii(bytes, 3U * 512U, "TONE    W01");
        le32(bytes, 3U * 512U + 0x1cU, 35U);
        std::fill_n(bytes.begin() + 4U * 512U, 512U, std::byte{});
        ascii(bytes, 4U * 512U, "LM8953");
        bytes[4U * 512U + 22U] = std::byte{0xc9};
        bytes[4U * 512U + 23U] = bytes[4U * 512U + 24U] = std::byte{2};
        const auto upload = uploads->create(
            {"owner", "tx16w.img", UploadKind::disk_image, "application/octet-stream", bytes.size(), {}});
        ASSERT_TRUE(upload);
        reference = upload->reference;
        ASSERT_TRUE(uploads->append(reference, "owner", 0U, bytes));
        ASSERT_TRUE(uploads->complete(reference, "owner"));
    }
    void TearDown() override {
        images.reset();
        uploads.reset();
        sandbox.reset();
        std::filesystem::remove_all(root);
    }
    Result<NativeDocumentSnapshot> open() {
        return conversion_internal::open_source(
            {{"kind", "SOURCES"},
             {"sources",
              nlohmann::json::array({{{"kind", "UPLOAD"}, {"upload", {{"uploadId", reference.upload_id}}}}})}},
            formats, "yamaha.tx16w", *sandbox, *uploads, *images, context);
    }
};

TEST_P(ConversionUploadVerification, ReplacedPathCannotApproveDifferentRetainedReaderBytes) {
    const auto opened = open();
    ASSERT_TRUE(opened) << opened.error().message;
    const auto path = uploads->resolve(reference, "owner");
    ASSERT_TRUE(path);
    const auto retained = root / "retained.img";
    std::error_code error;
    std::filesystem::rename(*path, retained, error);
    ASSERT_FALSE(error) << error.message();
    if (GetParam() == 2) {
        std::filesystem::create_symlink(retained, *path, error);
        if (error)
            GTEST_SKIP() << "Symlinks unavailable: " << error.message();
    } else {
        {
            std::ofstream replacement{*path, std::ios::binary};
            replacement.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        if (GetParam() == 1) {
            std::fstream changed{retained, std::ios::binary | std::ios::in | std::ios::out};
            changed.seekp(4U * 512U + 32U);
            changed.put('\x7f');
        }
    }
    EXPECT_FALSE(opened->document->sources()->verify());
    const auto document = std::dynamic_pointer_cast<const axk::tx16w::Document>(opened->document);
    ASSERT_TRUE(document);
    ASSERT_EQ(document->native_objects().size(), 1U);
    EXPECT_FALSE(document->read_wave_frames(document->native_objects().front().id, 0U, 2U));
}

INSTANTIATE_TEST_SUITE_P(Identity, ConversionUploadVerification, testing::Values(0, 1, 2));

} // namespace
