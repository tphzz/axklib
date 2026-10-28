#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/catalog.hpp"
#include "axklib/sfs_allocation.hpp"
#include "axklib/writer.hpp"

namespace {
class InitializedMedia final : public axk::RandomAccessReader {
  public:
    explicit InitializedMedia(axk::HdsInitialization value) : initialization(std::move(value)) {}
    std::uint64_t size() const noexcept override { return initialization.size_bytes; }
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> bytes) const override {
        if (offset > size() || bytes.size() > size() - offset)
            return std::unexpected(axk::make_error(axk::ErrorCode::io_read_failed, axk::ErrorCategory::io, "bounds"));
        std::ranges::fill(bytes, std::byte{0xa5});
        for (const auto &write : initialization.writes) {
            const auto begin = std::max(offset, write.offset);
            const auto end = std::min(offset + bytes.size(), write.offset + write.bytes.size());
            if (begin < end)
                std::memcpy(bytes.data() + (begin - offset), write.bytes.data() + (begin - write.offset), end - begin);
        }
        return {};
    }
    axk::HdsInitialization initialization;
};
} // namespace

TEST(HdsInitialization, ExplicitlyInitializesEmptyMetadataOnNonzeroMediaWithoutWholeDiskBuffers) {
    for (const auto &[size, count] :
         std::vector<std::pair<std::uint64_t, std::uint8_t>>{{2U * 1024U * 1024U, 1U}, {axk::maximum_hds_size, 8U}}) {
        auto planned = axk::plan_hds_initialization(size, count);
        ASSERT_TRUE(planned) << planned.error().message;
        ASSERT_FALSE(planned->writes.empty());
        std::uint64_t written{};
        std::uint64_t end{};
        for (const auto &write : planned->writes) {
            EXPECT_GE(write.offset, end);
            EXPECT_EQ(write.offset % 512U, 0U);
            EXPECT_EQ(write.bytes.size() % 512U, 0U);
            end = write.offset + write.bytes.size();
            EXPECT_LE(end, size);
            written += write.bytes.size();
        }
        EXPECT_LT(written, 8U * 1024U * 1024U);
        const auto reader = std::make_shared<InitializedMedia>(std::move(*planned));
        const auto image = axk::open_image(reader, {});
        ASSERT_TRUE(image) << image.error().message;
        EXPECT_TRUE(image->diagnostics().empty());
        EXPECT_TRUE(image->backup_superblock_matches());
        ASSERT_EQ(image->partitions().size(), count);
        for (const auto &partition : image->partitions()) {
            EXPECT_TRUE(axk::allocation_is_safe_for_mutation(partition.allocation));
            EXPECT_TRUE(partition.diagnostics.empty());
        }
        const auto catalog = axk::build_object_catalog(*image);
        ASSERT_TRUE(catalog);
        EXPECT_TRUE(catalog->objects.empty());
        std::vector<std::byte> last(512U);
        ASSERT_TRUE(reader->read_exact_at(size - last.size(), last));
        EXPECT_TRUE(std::ranges::all_of(last, [](auto byte) { return byte == std::byte{0xa5}; }));
    }
    EXPECT_FALSE(axk::plan_hds_initialization(axk::maximum_hds_size, 1U));
    EXPECT_FALSE(axk::plan_hds_initialization(axk::maximum_hds_size + 512U, 8U));
}
