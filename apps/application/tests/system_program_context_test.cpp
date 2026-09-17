#include "system_program_context.hpp"

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"

namespace {

std::vector<std::byte> system_record(axk::SystemFileKind kind) {
    std::vector<std::byte> bytes(axk::system_file_record_size(kind));
    axk::ByteWriter writer{bytes};
    EXPECT_TRUE(writer.write_ascii_field(0U, 12U, "FSFSDEV3SPLX", std::byte{}));
    EXPECT_TRUE(writer.write_ascii_field(0x0cU, 4U, "PRF3", std::byte{}));
    EXPECT_TRUE(writer.write_be32(0x30U, kind == axk::SystemFileKind::a3000_system ? 0x21520531U : 0xdeadfaceU));
    bytes[0x64U] = std::byte{15};
    bytes[0x66U] = std::byte{3};
    if (kind == axk::SystemFileKind::a4000_a5000_system2) {
        bytes[0x3eU] = std::byte{1};
        bytes[0x8eU] = std::byte{1};
        for (std::size_t index = 0; index < 32U; ++index)
            bytes[0x90U + index] = static_cast<std::byte>(index + 1U);
    }
    return bytes;
}

void expect_invalid(const axk::DecodedSystemFile &decoded) {
    const auto result = axk::app::image_sessions_internal::system_program_context(decoded);
    EXPECT_EQ(result.availability, axk::app::SystemProgramContextAvailability::invalid);
    EXPECT_FALSE(result.basic_receive);
    EXPECT_FALSE(result.saved_program_mode);
    EXPECT_FALSE(result.omni);
    EXPECT_FALSE(result.program_change_enabled);
    EXPECT_TRUE(result.parts.empty());
    EXPECT_FALSE(result.message.empty());
}

TEST(SystemProgramContextTest, ProjectsCompleteNativeReceiveContextWithoutMultiAssignments) {
    const auto decoded =
        axk::decode_system_file(axk::SystemFileKind::a3000_system, system_record(axk::SystemFileKind::a3000_system));
    ASSERT_TRUE(decoded);
    const auto result = axk::app::image_sessions_internal::system_program_context(*decoded);
    EXPECT_EQ(result.availability, axk::app::SystemProgramContextAvailability::available);
    ASSERT_TRUE(result.basic_receive);
    EXPECT_EQ(result.basic_receive->display, "16");
    EXPECT_EQ(result.omni, true);
    EXPECT_EQ(result.program_change_enabled, true);
    EXPECT_FALSE(result.saved_program_mode);
    EXPECT_TRUE(result.parts.empty());
}

TEST(SystemProgramContextTest, ProjectsBothPortsAndFalseMasterValuesForBothStorageRevisions) {
    for (const auto revision : {0U, 1U}) {
        auto bytes = system_record(axk::SystemFileKind::a4000_a5000_system2);
        bytes[0x3eU] = static_cast<std::byte>(revision);
        bytes[0x64U] = std::byte{31};
        const auto decoded = axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, bytes);
        ASSERT_TRUE(decoded);
        const auto result = axk::app::image_sessions_internal::system_program_context(*decoded);
        EXPECT_EQ(result.availability, axk::app::SystemProgramContextAvailability::available);
        EXPECT_EQ(result.storage_revision, revision);
        ASSERT_TRUE(result.basic_receive);
        EXPECT_EQ(result.basic_receive->display, "B16");
        EXPECT_EQ(result.saved_program_mode, "MULTI");
        ASSERT_EQ(result.parts.size(), 32U);
        EXPECT_EQ(result.parts.front().part_label, "A01");
        EXPECT_FALSE(result.parts.front().master);
        EXPECT_EQ(result.parts.back().part_label, "B16");
        EXPECT_TRUE(result.parts.back().master);
        EXPECT_EQ(result.parts.back().program_number, 32U);
    }
}

TEST(SystemProgramContextTest, DoesNotInventContextForInvalidSavedRoutingLeaves) {
    for (const auto kind : {axk::SystemFileKind::a3000_system, axk::SystemFileKind::a4000_a5000_system2}) {
        auto bytes = system_record(kind);
        bytes[0x64U] = std::byte{255};
        const auto decoded = axk::decode_system_file(kind, bytes);
        ASSERT_TRUE(decoded);
        expect_invalid(*decoded);
    }
    for (const auto offset : {0x8eU, 0x90U, 0xafU}) {
        auto bytes = system_record(axk::SystemFileKind::a4000_a5000_system2);
        bytes[offset] = std::byte{255};
        const auto decoded = axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, bytes);
        ASSERT_TRUE(decoded);
        expect_invalid(*decoded);
    }
}

} // namespace
