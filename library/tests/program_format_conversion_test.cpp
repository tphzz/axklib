#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/object.hpp"
#include "axklib/program_format_conversion.hpp"

namespace {

using Format = axk::ProgramStorageFormat;

// A4000 MAIN 1.07, address 0x1a0924. Neither fixture uses a production serializer.
constexpr std::array<std::uint8_t, 176> current_tail{
    0x01, 0x7f, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x7f, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x7f, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5b, 0x08, 0x01, 0x20, 0x5d, 0x1a,
    0x01, 0x20, 0x5e, 0x2c, 0x01, 0x20, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01, 0x40, 0x00,
    0x40, 0x00, 0x01, 0x40, 0x00, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
    0x40, 0x40, 0x40, 0x40, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

std::vector<std::byte> program_bytes(std::uint32_t revision, std::uint16_t count, std::size_t capacity) {
    const bool current = revision == 4U;
    const auto tail = 0x120U + capacity * 0x38U;
    const auto size = tail + (current ? 0xb0U : 0U);
    std::vector<std::byte> bytes(size);
    axk::ByteWriter writer{bytes};
    EXPECT_TRUE(writer.write_ascii_field(0, 16U, "FSFSDEV3SPLXPROG", std::byte{}));
    EXPECT_TRUE(writer.write_be32(0x14U, revision));
    EXPECT_TRUE(writer.write_be32(0x18U, static_cast<std::uint32_t>(size - (current ? 0xe0U : 0x30U))));
    EXPECT_TRUE(writer.write_be32(0x1cU, current ? static_cast<std::uint32_t>(size - 0x30U) : 0U));
    EXPECT_TRUE(writer.write_ascii_field(0x32U, 16U, "113", std::byte{' '}));
    EXPECT_TRUE(writer.write_ascii_field(0x78U, 8U, "RAWPROG", std::byte{' '}));
    bytes[0x30U] = std::byte{0x14};
    bytes[0x31U] = std::byte{0x0c};
    constexpr std::array<std::uint8_t, 22> common{0,   5, 0xff, 0xff, 0,    0, 0,  1,  64, 0,   64,
                                                  127, 0, 0,    0,    0xfe, 0, 90, 90, 39, 120, 0xff};
    std::ranges::transform(common, bytes.begin() + 0x80, [](auto value) { return static_cast<std::byte>(value); });
    EXPECT_TRUE(writer.write_be16(0x96U, count));
    for (std::size_t effect = 0; effect < 3U; ++effect) {
        const auto offset = 0x98U + effect * 0x28U;
        bytes[offset] = std::byte{1};
        bytes[offset + 1U] = bytes[offset + 2U] = std::byte{127};
    }
    for (std::size_t index = 0; index < 16U; ++index)
        bytes[0x110U + index] = static_cast<std::byte>(current_tail[0x78U + index]);
    for (std::size_t index = 0; index < capacity; ++index) {
        const auto row = 0x120U + index * 0x38U;
        EXPECT_TRUE(writer.write_ascii_field(row, 16U, "Target" + std::to_string(index), std::byte{' '}));
        EXPECT_TRUE(writer.write_be32(row + 0x10U, static_cast<std::uint32_t>(0x12340000U + index)));
        bytes[row + 0x14U] = std::byte{0x10};
        for (const auto offset : {0x15U, 0x23U, 0x24U, 0x2dU, 0x30U})
            bytes[row + offset] = std::byte{0xff};
        bytes[row + 0x1eU] = bytes[row + 0x21U] = std::byte{127};
        bytes[row + 0x33U] = std::byte{1};
        if (current)
            bytes[row + 0x1dU] = bytes[row + 0x28U] = std::byte{0xff};
    }
    if (current)
        std::ranges::transform(current_tail, bytes.begin() + static_cast<std::ptrdiff_t>(tail),
                               [](auto value) { return static_cast<std::byte>(value); });
    return bytes;
}

void expect_blocked(std::span<const std::byte> bytes, Format target) {
    const auto plan = axk::plan_program_format_conversion(bytes, target);
    EXPECT_FALSE(plan.allowed());
    EXPECT_FALSE(plan.no_op);
    EXPECT_TRUE(plan.converted_payload.empty());
    ASSERT_FALSE(plan.blockers.empty());
    for (const auto &issue : plan.blockers) {
        EXPECT_FALSE(issue.key.empty());
        EXPECT_FALSE(issue.message.empty());
    }
}

void expect_invalid_storage(std::span<const std::byte> bytes) {
    const auto info = axk::inspect_program_storage(bytes);
    EXPECT_FALSE(info.structurally_valid);
    EXPECT_FALSE(info.diagnostics.empty());
    expect_blocked(bytes, Format::a3000);
    expect_blocked(bytes, Format::a4000_a5000);
}

} // namespace

TEST(ProgramFormat, InspectionUsesHeaderRevisionCountAndCapacityNotAllocationPadding) {
    constexpr std::array<std::pair<std::uint16_t, std::size_t>, 6> dimensions{
        {{0U, 0U}, {0U, 8U}, {1U, 1U}, {3U, 9U}, {9U, 9U}, {999U, 999U}}};
    for (const std::uint32_t revision : {1U, 2U, 4U}) {
        for (const auto &[count, capacity] : dimensions) {
            SCOPED_TRACE(std::to_string(revision) + ":" + std::to_string(count));
            auto bytes = program_bytes(revision, count, capacity);
            const auto logical_size = bytes.size();
            bytes.resize(bytes.size() + 513U, std::byte{0xa5});
            const auto info = axk::inspect_program_storage(bytes);
            ASSERT_TRUE(info.structurally_valid);
            EXPECT_EQ(info.format, revision == 4U ? Format::a4000_a5000 : Format::a3000);
            EXPECT_EQ(info.header_revision, revision);
            EXPECT_EQ(info.logical_size, logical_size);
            EXPECT_EQ(info.stored_assignment_count, count);
            EXPECT_EQ(info.assignment_capacity, capacity);
            EXPECT_EQ(info.parameter_tail_bytes, revision == 4U ? 176U : 0U);
            EXPECT_TRUE(info.diagnostics.empty());
        }
    }
}

TEST(ProgramFormat, RejectsMalformedIdentityHeaderLengthsCountsAndPartialRows) {
    for (const std::uint32_t revision : {1U, 2U, 4U}) {
        const auto original = program_bytes(revision, 1U, 8U);
        for (const auto length : {0U, 0x14U, 0x78U, 0x11fU})
            expect_invalid_storage(std::span{original}.first(length));
        auto bytes = original;
        bytes.pop_back();
        expect_invalid_storage(bytes);
        bytes = original;
        bytes[0] = std::byte{'X'};
        expect_invalid_storage(bytes);
        bytes = original;
        bytes[0x0cU] = std::byte{'S'};
        expect_invalid_storage(bytes);
        bytes = original;
        bytes[0x30U] = std::byte{0x10};
        expect_invalid_storage(bytes);
        bytes = original;
        ASSERT_TRUE(axk::ByteWriter{bytes}.write_be32(0x18U, 0U));
        expect_invalid_storage(bytes);
        bytes = original;
        bytes.push_back(std::byte{});
        ASSERT_TRUE(axk::ByteWriter{bytes}.write_be32(
            0x18U, static_cast<std::uint32_t>(bytes.size() - (revision == 4U ? 0xe0U : 0x30U))));
        if (revision == 4U)
            ASSERT_TRUE(axk::ByteWriter{bytes}.write_be32(0x1cU, static_cast<std::uint32_t>(bytes.size() - 0x30U)));
        expect_invalid_storage(bytes);
        expect_invalid_storage(program_bytes(revision, 9U, 8U));
        expect_invalid_storage(program_bytes(revision, 1000U, 1000U));
    }
    for (const std::uint32_t revision : {0U, 3U, 5U, 0xffffffffU}) {
        auto bytes = program_bytes(4U, 0U, 0U);
        ASSERT_TRUE(axk::ByteWriter{bytes}.write_be32(0x14U, revision));
        expect_invalid_storage(bytes);
    }
    auto current = program_bytes(4U, 0U, 0U);
    ASSERT_TRUE(axk::ByteWriter{current}.write_be32(0x1cU, 0U));
    expect_invalid_storage(current);
}

TEST(ProgramFormat, SameStoredFormatIsByteExactEvenForUninterpretableParameters) {
    for (const std::uint32_t revision : {1U, 2U, 4U}) {
        auto bytes = program_bytes(revision, 1U, 9U);
        bytes[0x8bU] = std::byte{255};
        bytes[0x9eU] = bytes[0x9fU] = std::byte{255};
        bytes[0x111U] = std::byte{255};
        bytes[0x143U] = std::byte{0xaa};
        if (revision == 4U) {
            const auto tail = bytes.size() - 0xb0U;
            bytes[tail + 0x79U] = std::byte{255};
            bytes[tail + 0xa6U] = bytes[tail + 0xafU] = std::byte{255};
        }
        bytes.resize(bytes.size() + 71U, std::byte{0xd3});
        const auto original = bytes;
        const auto format = revision == 4U ? Format::a4000_a5000 : Format::a3000;
        const auto plan = axk::plan_program_format_conversion(bytes, format);
        ASSERT_TRUE(plan.allowed());
        EXPECT_TRUE(plan.no_op);
        EXPECT_EQ(plan.target, format);
        EXPECT_EQ(plan.source.format, format);
        EXPECT_TRUE(plan.changes.empty());
        EXPECT_TRUE(plan.blockers.empty());
        EXPECT_EQ(plan.converted_payload, original);
        EXPECT_EQ(bytes, original);
    }
}

TEST(ProgramFormat, DecoderRejectsAProgramSignatureWithAnotherCommonClass) {
    for (const std::uint32_t revision : {1U, 2U, 4U}) {
        auto bytes = program_bytes(revision, 1U, 8U);
        bytes[0x30U] = std::byte{0x10};
        EXPECT_FALSE(axk::decode_object(bytes));
        expect_invalid_storage(bytes);
    }
}

TEST(ProgramFormat, DowngradeMustNotDiscardRetainedCrossfadeBits) {
    constexpr std::array<std::pair<std::byte, std::byte>, 3> encodings{
        {{std::byte{0xfb}, std::byte{0x3f}}, {std::byte{5}, std::byte{0x7f}}, {std::byte{}, std::byte{0xff}}}};
    for (const auto &[offset, packed] : encodings) {
        for (const auto retained : {std::byte{0x3f}, std::byte{0x7f}, std::byte{0xbf}, std::byte{0xff}}) {
            auto bytes = program_bytes(4U, 1U, 2U);
            bytes[0x139U] = bytes[0x13bU] = offset;
            bytes[0x143U] = retained;
            const auto original = bytes;
            const auto plan = axk::plan_program_format_conversion(bytes, Format::a3000);
            if (retained == packed) {
                ASSERT_TRUE(plan.allowed()) << (plan.blockers.empty() ? "" : plan.blockers.front().message);
                EXPECT_EQ(plan.converted_payload[0x143U], retained);
            } else {
                expect_blocked(bytes, Format::a3000);
            }
            EXPECT_EQ(bytes, original);
        }
    }
}

TEST(ProgramFormat, UnknownTargetNeverProducesAConvertedPayload) {
    for (const std::uint32_t revision : {1U, 2U, 4U})
        expect_blocked(program_bytes(revision, 0U, 0U), Format::unknown);
}

TEST(ProgramFormat, NativeUpgradeAddsTheCompleteParameterTailAndPreservesPadding) {
    auto bytes = program_bytes(2U, 0U, 0U);
    bytes.resize(bytes.size() + 73U, std::byte{0xc7});
    const auto original = bytes;
    auto expected = program_bytes(4U, 0U, 0U);
    expected.resize(expected.size() + 73U, std::byte{0xc7});
    const auto plan = axk::plan_program_format_conversion(bytes, Format::a4000_a5000);
    ASSERT_TRUE(plan.allowed()) << (plan.blockers.empty() ? "" : plan.blockers.front().message);
    EXPECT_FALSE(plan.no_op);
    EXPECT_FALSE(plan.changes.empty());
    EXPECT_EQ(plan.source.format, Format::a3000);
    EXPECT_EQ(plan.source.header_revision, 2U);
    EXPECT_EQ(plan.target, Format::a4000_a5000);
    EXPECT_EQ(plan.converted_payload, expected);
    EXPECT_EQ(bytes, original);
}

TEST(ProgramFormat, UpgradeProjectsControllersAndBothAdChannelsFromLegacyFields) {
    auto bytes = program_bytes(2U, 0U, 8U);
    constexpr std::array<std::uint8_t, 16> controls{71, 63, 3, 0xc1, 125, 1, 0, 63, 2, 4, 1, 32, 3, 5, 2, 0};
    std::ranges::transform(controls, bytes.begin() + 0x110, [](auto value) { return static_cast<std::byte>(value); });
    bytes[0x86U] = std::byte{0xeb};
    bytes[0x87U] = std::byte{4};
    bytes[0x88U] = std::byte{97};
    bytes[0x89U] = std::byte{5};
    bytes[0x8aU] = std::byte{83};
    const auto plan = axk::plan_program_format_conversion(bytes, Format::a4000_a5000);
    ASSERT_TRUE(plan.allowed()) << (plan.blockers.empty() ? "" : plan.blockers.front().message);
    const auto &converted = plan.converted_payload;
    const auto tail = 0x120U + 8U * 0x38U;
    EXPECT_TRUE(
        std::ranges::equal(std::span{converted}.subspan(tail + 0x78U, 16U), std::span{bytes}.subspan(0x110U, 16U)));
    EXPECT_TRUE(std::ranges::equal(std::span{converted}.subspan(0x110U, 16U), std::span{bytes}.subspan(0x110U, 16U)));
    for (const auto output : {tail + 0x8dU, tail + 0x92U}) {
        EXPECT_EQ(converted[output], std::byte{4});
        EXPECT_EQ(converted[output + 1U], std::byte{97});
        EXPECT_EQ(converted[output + 2U], std::byte{5});
        EXPECT_EQ(converted[output + 3U], std::byte{83});
    }
    EXPECT_EQ(converted[tail + 0x91U], std::byte{0xeb});
    EXPECT_TRUE(std::ranges::equal(std::span{converted}.subspan(0x80U, 22U), std::span{bytes}.subspan(0x80U, 22U)));
}

TEST(ProgramFormat, UpgradeTransformsOnlyCountedRowsAndPreservesIdentityOpaqueBytesAndDormantCapacity) {
    auto bytes = program_bytes(2U, 3U, 9U);
    for (const auto offset : {0x42U, 0x49U, 0x4aU, 0x64U, 0x68U, 0x74U})
        bytes[offset] = std::byte{0xa9};
    const auto dormant = 0x120U + 3U * 0x38U;
    std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(dormant), bytes.end(), std::byte{0xa5});
    constexpr std::array<std::uint8_t, 3> flags{0x3f, 0x7f, 0xff};
    constexpr std::array<std::uint8_t, 3> crossfade{0xfb, 5, 0};
    for (std::size_t index = 0; index < 3U; ++index) {
        const auto row = 0x120U + index * 0x38U;
        bytes[row + 0x23U] = static_cast<std::byte>(flags[index]);
        bytes[row + 0x2dU] = std::byte{4};
        bytes[row + 0x2eU] = std::byte{19};
        bytes[row + 0x30U] = std::byte{5};
        bytes[row + 0x31U] = std::byte{0xec};
    }
    bytes.resize(bytes.size() + 19U, std::byte{0xd7});
    const auto original = bytes;
    const auto plan = axk::plan_program_format_conversion(bytes, Format::a4000_a5000);
    ASSERT_TRUE(plan.allowed()) << (plan.blockers.empty() ? "" : plan.blockers.front().message);
    const auto &converted = plan.converted_payload;
    for (std::size_t index = 0; index < 3U; ++index) {
        const auto row = 0x120U + index * 0x38U;
        auto expected_row = std::vector<std::byte>{bytes.begin() + static_cast<std::ptrdiff_t>(row),
                                                   bytes.begin() + static_cast<std::ptrdiff_t>(row + 0x38U)};
        expected_row[0x19U] = expected_row[0x1bU] = static_cast<std::byte>(crossfade[index]);
        expected_row[0x1dU] = std::byte{4};
        expected_row[0x2fU] = std::byte{19};
        expected_row[0x28U] = std::byte{5};
        expected_row[0x32U] = std::byte{0xec};
        EXPECT_TRUE(std::ranges::equal(std::span{converted}.subspan(row, 0x38U), expected_row));
    }
    EXPECT_TRUE(std::ranges::equal(std::span{converted}.subspan(0x30U, 0x68U), std::span{bytes}.subspan(0x30U, 0x68U)));
    EXPECT_TRUE(std::ranges::equal(std::span{converted}.subspan(dormant, 6U * 0x38U),
                                   std::span{bytes}.subspan(dormant, 6U * 0x38U)));
    EXPECT_TRUE(std::ranges::equal(std::span{converted}.last(19U), std::span{bytes}.last(19U)));
    const auto info = axk::inspect_program_storage(converted);
    EXPECT_TRUE(info.structurally_valid);
    EXPECT_EQ(info.stored_assignment_count, 3U);
    EXPECT_EQ(info.assignment_capacity, 9U);
    EXPECT_EQ(bytes, original);
}

TEST(ProgramFormat, NativeUnsupportedCrossfadeAndOpaqueEffectTypeAreNotSilentlyOverwritten) {
    for (const std::uint32_t revision : {1U, 2U}) {
        auto bytes = program_bytes(revision, 1U, 1U);
        bytes[0x143U] = std::byte{0xbf};
        expect_blocked(bytes, Format::a4000_a5000);
        for (std::size_t slot = 0; slot < 3U; ++slot) {
            bytes = program_bytes(revision, 1U, 1U);
            bytes[0x98U + slot * 0x28U + 6U] = std::byte{1};
            expect_blocked(bytes, Format::a4000_a5000);
        }
        bytes = program_bytes(revision, 1U, 1U);
        bytes[0x80U] = std::byte{0x80};
        expect_blocked(bytes, Format::a4000_a5000);
    }
}

TEST(ProgramFormat, NativeUpgradeRefusesNonzeroOpaqueLanesThatPromotionWouldOverwrite) {
    for (const std::uint32_t revision : {1U, 2U}) {
        for (const auto offset : {0x19U, 0x1bU, 0x1dU, 0x28U, 0x2fU, 0x32U}) {
            SCOPED_TRACE(std::to_string(revision) + ":" + std::to_string(offset));
            auto bytes = program_bytes(revision, 1U, 1U);
            bytes[0x120U + offset] = std::byte{1};
            expect_blocked(bytes, Format::a4000_a5000);
        }
    }
}

TEST(ProgramFormat, BothDirectionsRefuseUnrepresentableVelocitySensitivityAndControllerFlag) {
    for (const std::uint32_t revision : {1U, 2U, 4U}) {
        for (const auto &[offset, value] :
             std::array<std::pair<std::size_t, std::byte>, 2>{{{0x17U, std::byte{1}}, {0x34U, std::byte{0x20}}}}) {
            SCOPED_TRACE(std::to_string(revision) + ":" + std::to_string(offset));
            auto bytes = program_bytes(revision, 1U, 1U);
            bytes[0x120U + offset] = value;
            expect_blocked(bytes, revision == 4U ? Format::a3000 : Format::a4000_a5000);
        }
    }
}

TEST(ProgramFormat, ConversionGuardsDoNotInterpretUncountedRows) {
    for (const std::uint32_t revision : {1U, 2U, 4U}) {
        auto bytes = program_bytes(revision, 1U, 2U);
        std::fill_n(bytes.begin() + 0x158, 0x38U, std::byte{0xff});
        const auto plan =
            axk::plan_program_format_conversion(bytes, revision == 4U ? Format::a3000 : Format::a4000_a5000);
        ASSERT_TRUE(plan.allowed()) << (plan.blockers.empty() ? "" : plan.blockers.front().message);
        EXPECT_TRUE(std::ranges::equal(std::span{plan.converted_payload}.subspan(0x158U, 0x38U),
                                       std::span{bytes}.subspan(0x158U, 0x38U)));
    }
}

TEST(ProgramFormat, NativeUpgradeRefusesAnUnexplainedExtensionLengthWithoutChangingNoOpBehavior) {
    for (const std::uint32_t revision : {1U, 2U}) {
        auto bytes = program_bytes(revision, 1U, 1U);
        ASSERT_TRUE(axk::ByteWriter{bytes}.write_be32(0x1cU, 1U));
        ASSERT_TRUE(axk::inspect_program_storage(bytes).structurally_valid);
        expect_blocked(bytes, Format::a4000_a5000);
        const auto same = axk::plan_program_format_conversion(bytes, Format::a3000);
        ASSERT_TRUE(same.allowed());
        EXPECT_TRUE(same.no_op);
        EXPECT_EQ(same.converted_payload, bytes);
    }
}

TEST(ProgramFormat, VersionOneRemovedEffectTypesAreRefusedRatherThanSilentlyReplaced) {
    for (const auto type : {47U, 48U, 49U, 50U, 51U}) {
        for (std::size_t slot = 0; slot < 3U; ++slot) {
            SCOPED_TRACE(std::to_string(type) + ":" + std::to_string(slot));
            auto bytes = program_bytes(1U, 0U, 0U);
            bytes[0x98U + slot * 0x28U + 7U] = static_cast<std::byte>(type);
            expect_blocked(bytes, Format::a4000_a5000);
        }
    }
}

TEST(ProgramFormat, UpgradeNormalizesVersionOneTypesBeforePromotingAllThreeEffectBlocks) {
    constexpr std::array<std::uint16_t, 16> hall{49, 18, 10, 8, 13, 49, 0, 4, 50, 8, 64, 5, 5, 5, 5, 5};
    for (const std::uint32_t revision : {1U, 2U}) {
        auto bytes = program_bytes(revision, 0U, 0U);
        for (std::size_t slot = 0; slot < 3U; ++slot) {
            const auto effect = 0x98U + slot * 0x28U;
            bytes[effect + 7U] = revision == 1U ? std::byte{52} : std::byte{47};
            for (std::size_t parameter = 0; parameter < hall.size(); ++parameter)
                ASSERT_TRUE(axk::ByteWriter{bytes}.write_be16(effect + 8U + parameter * 2U, hall[parameter]));
        }
        const auto plan = axk::plan_program_format_conversion(bytes, Format::a4000_a5000);
        ASSERT_TRUE(plan.allowed()) << (plan.blockers.empty() ? "" : plan.blockers.front().message);
        for (std::size_t slot = 0; slot < 3U; ++slot) {
            const auto effect = 0x98U + slot * 0x28U;
            EXPECT_EQ(plan.converted_payload[effect + 6U], std::byte{47});
            EXPECT_EQ(plan.converted_payload[effect + 7U], std::byte{47});
            EXPECT_TRUE(std::ranges::equal(std::span{plan.converted_payload}.subspan(effect + 8U, 32U),
                                           std::span{bytes}.subspan(effect + 8U, 32U)));
        }
    }
}

TEST(ProgramFormat, LosslessCurrentRoundTripPreservesCountCapacityIdentityHandlesAndPadding) {
    constexpr std::array<std::pair<std::uint16_t, std::size_t>, 5> dimensions{
        {{0U, 0U}, {0U, 8U}, {1U, 1U}, {3U, 9U}, {999U, 999U}}};
    for (const auto &[count, capacity] : dimensions) {
        SCOPED_TRACE(count);
        auto bytes = program_bytes(4U, count, capacity);
        bytes[0x42U] = std::byte{0xab};
        bytes[0x74U] = std::byte{0xef};
        const auto dormant = 0x120U + static_cast<std::size_t>(count) * 0x38U;
        const auto tail = 0x120U + capacity * 0x38U;
        std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(dormant),
                  bytes.begin() + static_cast<std::ptrdiff_t>(tail), std::byte{0xa5});
        bytes.resize(bytes.size() + 37U, std::byte{0xc5});
        const auto down = axk::plan_program_format_conversion(bytes, Format::a3000);
        ASSERT_TRUE(down.allowed()) << (down.blockers.empty() ? "" : down.blockers.front().message);
        EXPECT_FALSE(down.no_op);
        EXPECT_EQ(down.converted_payload.size(), bytes.size() - 0xb0U);
        const auto info = axk::inspect_program_storage(down.converted_payload);
        EXPECT_TRUE(info.structurally_valid);
        EXPECT_EQ(info.format, Format::a3000);
        EXPECT_EQ(info.header_revision, 2U);
        EXPECT_EQ(info.stored_assignment_count, count);
        EXPECT_EQ(info.assignment_capacity, capacity);
        EXPECT_EQ(info.parameter_tail_bytes, 0U);
        const auto up = axk::plan_program_format_conversion(down.converted_payload, Format::a4000_a5000);
        ASSERT_TRUE(up.allowed()) << (up.blockers.empty() ? "" : up.blockers.front().message);
        EXPECT_EQ(up.converted_payload, bytes);
    }
}

TEST(ProgramFormat, NativeRoundTripPreservesOpaqueBytesAndNormalizesOnlyTheRevision) {
    for (const std::uint32_t revision : {1U, 2U}) {
        auto bytes = program_bytes(revision, 3U, 8U);
        for (const auto offset : {0x42U, 0x43U, 0x49U, 0x4aU, 0x64U, 0x68U, 0x74U})
            bytes[offset] = std::byte{0xa9};
        constexpr std::array<std::byte, 3> flags{std::byte{0x3f}, std::byte{0x7f}, std::byte{0xff}};
        for (std::size_t index = 0; index < 3U; ++index) {
            const auto row = 0x120U + index * 0x38U;
            bytes[row + 0x23U] = flags[index];
            bytes[row + 0x36U] = std::byte{0x5a};
            bytes[row + 0x37U] = std::byte{0xa5};
        }
        std::fill(bytes.begin() + 0x1c8, bytes.end(), std::byte{0xa5});
        bytes.resize(bytes.size() + 71U, std::byte{0xd3});
        const auto up = axk::plan_program_format_conversion(bytes, Format::a4000_a5000);
        ASSERT_TRUE(up.allowed()) << (up.blockers.empty() ? "" : up.blockers.front().message);
        const auto down = axk::plan_program_format_conversion(up.converted_payload, Format::a3000);
        ASSERT_TRUE(down.allowed()) << (down.blockers.empty() ? "" : down.blockers.front().message);
        auto expected = bytes;
        ASSERT_TRUE(axk::ByteWriter{expected}.write_be32(0x14U, 2U));
        EXPECT_EQ(down.converted_payload, expected);
    }
}

TEST(ProgramFormat, DowngradeUsesCanonicalStateWhenEveryCompatibilityCopyIsStale) {
    auto bytes = program_bytes(4U, 1U, 1U);
    const auto tail = bytes.size() - 0xb0U;
    std::fill_n(bytes.begin() + 0x87, 4U, std::byte{0xfe});
    std::fill_n(bytes.begin() + 0x110, 16U, std::byte{0xee});
    for (std::size_t slot = 0; slot < 3U; ++slot)
        bytes[0x98U + slot * 0x28U + 7U] = std::byte{0xff};
    for (const auto offset : {0x2dU, 0x2eU, 0x30U, 0x31U})
        bytes[0x120U + offset] = std::byte{0xfd};
    const auto original = bytes;
    const auto down = axk::plan_program_format_conversion(bytes, Format::a3000);
    ASSERT_TRUE(down.allowed()) << (down.blockers.empty() ? "" : down.blockers.front().message);
    const auto &converted = down.converted_payload;
    EXPECT_EQ(converted[0x87U], std::byte{1});
    EXPECT_EQ(converted[0x88U], std::byte{64});
    EXPECT_EQ(converted[0x89U], std::byte{});
    EXPECT_EQ(converted[0x8aU], std::byte{64});
    EXPECT_TRUE(
        std::ranges::equal(std::span{converted}.subspan(0x110U, 16U), std::span{bytes}.subspan(tail + 0x78U, 16U)));
    for (std::size_t slot = 0; slot < 3U; ++slot)
        EXPECT_EQ(converted[0x98U + slot * 0x28U + 7U], std::byte{});
    EXPECT_EQ(converted[0x14dU], std::byte{0xff});
    EXPECT_EQ(converted[0x14eU], std::byte{});
    EXPECT_EQ(converted[0x150U], std::byte{0xff});
    EXPECT_EQ(converted[0x151U], std::byte{});
    EXPECT_EQ(bytes, original);
}

TEST(ProgramFormat, ExplicitOffOutputsRetainNonzeroDormantLevelsThroughRoundTrip) {
    auto bytes = program_bytes(4U, 1U, 1U);
    const auto tail = bytes.size() - 0xb0U;
    bytes[0x87U] = bytes[0x89U] = std::byte{};
    bytes[0x88U] = std::byte{37};
    bytes[0x8aU] = std::byte{19};
    for (const auto output : {tail + 0x8dU, tail + 0x92U}) {
        bytes[output] = bytes[output + 2U] = std::byte{};
        bytes[output + 1U] = std::byte{37};
        bytes[output + 3U] = std::byte{19};
    }
    for (const auto offset : {0x1dU, 0x28U, 0x2dU, 0x30U})
        bytes[0x120U + offset] = std::byte{};
    bytes[0x14eU] = bytes[0x14fU] = std::byte{37};
    bytes[0x151U] = bytes[0x152U] = std::byte{0xed};
    const auto down = axk::plan_program_format_conversion(bytes, Format::a3000);
    ASSERT_TRUE(down.allowed()) << (down.blockers.empty() ? "" : down.blockers.front().message);
    EXPECT_EQ(down.converted_payload[0x88U], std::byte{37});
    EXPECT_EQ(down.converted_payload[0x8aU], std::byte{19});
    EXPECT_EQ(down.converted_payload[0x14eU], std::byte{37});
    EXPECT_EQ(down.converted_payload[0x151U], std::byte{0xed});
    const auto up = axk::plan_program_format_conversion(down.converted_payload, Format::a4000_a5000);
    ASSERT_TRUE(up.allowed()) << (up.blockers.empty() ? "" : up.blockers.front().message);
    EXPECT_EQ(up.converted_payload, bytes);
}

TEST(ProgramFormat, DowngradeAcceptsTheHighestNativeEffectTypeAndControllerFunction) {
    auto bytes = program_bytes(4U, 1U, 1U);
    const auto tail = bytes.size() - 0xb0U;
    constexpr std::array<std::uint16_t, 16> canyon{50, 59, 6, 63, 0, 45, 34, 62, 91, 13, 25, 4, 64, 4, 64, 0};
    bytes[0x9eU] = bytes[0x9fU] = std::byte{54};
    for (std::size_t parameter = 0; parameter < canyon.size(); ++parameter)
        ASSERT_TRUE(axk::ByteWriter{bytes}.write_be16(0xa0U + parameter * 2U, canyon[parameter]));
    for (const auto offset : {std::size_t{0x110U}, tail + 0x78U}) {
        bytes[offset] = std::byte{125};
        bytes[offset + 1U] = std::byte{63};
        bytes[offset + 2U] = std::byte{3};
        bytes[offset + 3U] = std::byte{63};
    }
    const auto down = axk::plan_program_format_conversion(bytes, Format::a3000);
    ASSERT_TRUE(down.allowed()) << (down.blockers.empty() ? "" : down.blockers.front().message);
    EXPECT_EQ(down.converted_payload[0x9eU], std::byte{});
    EXPECT_EQ(down.converted_payload[0x9fU], std::byte{54});
    EXPECT_EQ(down.converted_payload[0x111U], std::byte{63});
    const auto up = axk::plan_program_format_conversion(down.converted_payload, Format::a4000_a5000);
    ASSERT_TRUE(up.allowed()) << (up.blockers.empty() ? "" : up.blockers.front().message);
    EXPECT_EQ(up.converted_payload, bytes);
}

TEST(ProgramFormat, DowngradeRefusesCurrentOnlyEffectsControllersAndMidiPortB) {
    for (std::size_t slot = 0; slot < 3U; ++slot) {
        for (const auto type : {55U, 96U}) {
            auto bytes = program_bytes(4U, 1U, 1U);
            bytes[0x98U + slot * 0x28U + 6U] = static_cast<std::byte>(type);
            expect_blocked(bytes, Format::a3000);
        }
    }
    for (std::size_t index = 0; index < 4U; ++index) {
        auto bytes = program_bytes(4U, 1U, 1U);
        const auto tail = bytes.size() - 0xb0U;
        bytes[tail + 0x79U + index * 4U] = std::byte{64};
        expect_blocked(bytes, Format::a3000);
    }
    for (const auto offset : {0x88U, 0x89U, 0x8aU, 0x8bU}) {
        auto bytes = program_bytes(4U, 1U, 1U);
        const auto tail = bytes.size() - 0xb0U;
        bytes[tail + offset] ^= std::byte{1};
        expect_blocked(bytes, Format::a3000);
    }
    for (const auto offset : {0x8fU, 0x135U}) {
        auto bytes = program_bytes(4U, 1U, 1U);
        bytes[offset] = std::byte{17};
        expect_blocked(bytes, Format::a3000);
    }
}

TEST(ProgramFormat, DowngradeRefusesIndependentAdRoutesAndUnrepresentableAssignmentOutputs) {
    for (const auto offset : {0x91U, 0x92U, 0x93U, 0x94U, 0x95U}) {
        auto bytes = program_bytes(4U, 1U, 1U);
        const auto tail = bytes.size() - 0xb0U;
        bytes[tail + offset] ^= std::byte{1};
        expect_blocked(bytes, Format::a3000);
    }
    for (const auto offset : {0x13dU, 0x148U}) {
        auto bytes = program_bytes(4U, 1U, 1U);
        bytes[offset] = std::byte{12};
        expect_blocked(bytes, Format::a3000);
    }
}

TEST(ProgramFormat, DowngradeRefusesStepWaveExtraEffectsAndNondefaultExtensionState) {
    constexpr std::array<std::size_t, 11> offsets{0U,    0x28U, 0x50U, 0x8cU, 0x96U, 0xa5U,
                                                  0xa6U, 0xa7U, 0xa8U, 0xaeU, 0xafU};
    for (const auto offset : offsets) {
        SCOPED_TRACE(offset);
        auto bytes = program_bytes(4U, 1U, 1U);
        const auto tail = bytes.size() - 0xb0U;
        bytes[tail + offset] ^= std::byte{1};
        expect_blocked(bytes, Format::a3000);
    }
    for (const auto offset : {0x8cU, 0xa6U}) {
        auto bytes = program_bytes(4U, 1U, 1U);
        const auto tail = bytes.size() - 0xb0U;
        bytes[tail + offset] |= std::byte{0x80};
        expect_blocked(bytes, Format::a3000);
    }
    auto bytes = program_bytes(4U, 1U, 1U);
    bytes[0x81U] = std::byte{0x35};
    expect_blocked(bytes, Format::a3000);
}
