#include <array>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/writer_internal.hpp"

TEST(SfsWriteCodec, EncodesDirectDirectoryRecordWithCheckedFields) {
    axk::detail::PreparedRecord record;
    record.kind = axk::detail::RecordKind::directory;
    record.tail = 7U;
    record.cluster = 42U;
    record.clusters = 2U;
    record.payload.resize(1'500U);

    const auto encoded = axk::detail::encode_sfs_index_record(record);

    ASSERT_TRUE(encoded) << encoded.error().message;
    ASSERT_EQ(encoded->size(), axk::detail::sfs_directory_index_record_bytes);
    const axk::ByteReader reader{*encoded};
    ASSERT_TRUE(reader.be16(0U));
    EXPECT_EQ(*reader.be16(0U), 1U);
    EXPECT_EQ(*reader.be16(4U), 2U);
    EXPECT_EQ(*reader.be32(6U), 1'500U);
    EXPECT_EQ(*reader.be32(0x0aU), 42U);
    EXPECT_EQ(*reader.be32(0x0eU), 2U);
    EXPECT_EQ(*reader.be32(0x12U), 1'500U);
    EXPECT_EQ((*encoded)[0x42U], std::byte{0x94});
    EXPECT_EQ((*encoded)[0x43U], std::byte{'d'});
    EXPECT_EQ((*encoded)[0x44U], std::byte{'i'});
    EXPECT_EQ((*encoded)[0x45U], std::byte{'r'});
    EXPECT_EQ(*reader.be16(0x46U), 7U);
}

TEST(SfsWriteCodec, RejectsTooManyDirectExtentsBeforeWritingTheRecord) {
    axk::detail::PreparedRecord record;
    record.kind = axk::detail::RecordKind::object;
    const std::array extents{
        axk::Extent{10U, 1U, 1'024U}, axk::Extent{12U, 1U, 1'024U}, axk::Extent{14U, 1U, 1'024U},
        axk::Extent{16U, 1U, 1'024U}, axk::Extent{18U, 1U, 1'024U},
    };

    const auto encoded = axk::detail::encode_sfs_index_record(record, extents, 5U * 1'024U);

    ASSERT_FALSE(encoded);
    EXPECT_EQ(encoded.error().code, axk::ErrorCode::internal_invariant);
}
