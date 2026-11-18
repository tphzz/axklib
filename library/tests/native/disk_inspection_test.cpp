#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>

#include <gtest/gtest.h>

#include "axklib/formats/tx16w/document.hpp"
#include "axklib/inspection/disk.hpp"

namespace {
class HeaderOnlyReader final : public axk::RandomAccessReader {
  public:
    HeaderOnlyReader() {
        for (std::size_t i = 0U; i < 6U; ++i)
            header[i] = static_cast<std::byte>("LM8953"[i]);
        header[22] = std::byte{0xc9};
        header[23] = std::byte{2};
        header[24] = std::byte{2};
    }
    std::uint64_t size() const noexcept override { return 35U; }
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> output) const override {
        ++reads;
        if (offset > header.size() || output.size() > header.size() - offset)
            return std::unexpected(
                axk::make_error(axk::ErrorCode::out_of_bounds, axk::ErrorCategory::io, "Inspection requested PCM"));
        std::ranges::copy(std::span{header}.subspan(static_cast<std::size_t>(offset), output.size()), output.begin());
        return {};
    }
    mutable std::size_t reads{};

  private:
    std::array<std::byte, 32> header{};
};
} // namespace

TEST(NativeInspection, BrowsesAuthorizedEntryWithoutPcmOrRediscoveryBetweenPages) {
    const auto reader = std::make_shared<HeaderOnlyReader>();
    axk::native::SourceMember member{
        "drive", "floppy", {}, {{"fat-0", "FAT", {{"entry", "/TONE.W01", false, 0, reader}}}}};
    bool stale{};
    member.verify_member = [&](const auto &token) -> axk::Result<void> {
        if (stale)
            return std::unexpected(
                axk::make_error(axk::ErrorCode::transaction_stale, axk::ErrorCategory::io, "Drive changed"));
        return token.check();
    };
    member.verify_location = [verify = member.verify_member](const auto &, const auto &token) { return verify(token); };
    const auto sources = axk::native::SourceSet::create({std::move(member)});
    ASSERT_TRUE(sources);
    axk::native::FormatRegistry formats;
    ASSERT_TRUE(formats.add(axk::tx16w::provider()));
    auto opened = axk::inspection::DiskInspection::inspect(*sources, formats);
    ASSERT_TRUE(opened) << opened.error().message;
    const auto summary = opened->summary();
    const auto doc = summary["documents"].at(0).at("id").get<std::string>();
    const auto reads = reader->reads;
    ASSERT_GT(reads, 0U);
    const auto page = opened->objects(doc, 0U, 1U);
    ASSERT_TRUE(page);
    EXPECT_EQ(page->at("items").at(0).at("name"), "TONE.W01");
    EXPECT_TRUE(opened->objects(doc, 1U, 1U)->at("items").empty());
    EXPECT_TRUE(opened->relationships(doc, 0U, 1U)->at("items").empty());
    EXPECT_FALSE(opened->objects(doc, 0U, 0U));
    EXPECT_FALSE(opened->objects("wrong", 0U, 1U));
    axk::CancellationSource stop;
    stop.cancel();
    EXPECT_FALSE(opened->objects(doc, 0U, 1U, stop.token()));
    EXPECT_EQ(reader->reads, reads);
    stale = true;
    auto loc = (*sources)->locate("drive", "fat-0", "entry");
    ASSERT_TRUE(loc);
    loc->length = 1U;
    const auto denied = (*sources)->read(*loc);
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code, axk::ErrorCode::transaction_stale);
    EXPECT_EQ(reader->reads, reads);
}
