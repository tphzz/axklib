#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/catalog.hpp"
#include "axklib/object.hpp"
#include "axklib/package_relocation.hpp"
#include "axklib/relationship.hpp"
#include "axklib/writer_internal.hpp"

namespace {

// Independent disk fixtures: no production layout or serialization helpers.
std::vector<std::byte> program_bytes(std::uint32_t version, std::uint16_t count, std::size_t capacity) {
    const auto size = 0x120U + capacity * 0x38U + (version == 4U ? 0xb0U : 0U);
    std::vector<std::byte> bytes(size);
    axk::ByteWriter writer{bytes};
    EXPECT_TRUE(writer.write_ascii_field(0, 16, "FSFSDEV3SPLXPROG", std::byte{}));
    EXPECT_TRUE(writer.write_be32(0x14U, version));
    EXPECT_TRUE(writer.write_be32(0x18U, static_cast<std::uint32_t>(size - (version == 4U ? 0xe0U : 0x30U))));
    EXPECT_TRUE(writer.write_be32(0x1cU, version == 4U ? static_cast<std::uint32_t>(size - 0x30U) : 0U));
    EXPECT_TRUE(writer.write_ascii_field(0x32U, 16, "001", std::byte{' '}));
    EXPECT_TRUE(writer.write_ascii_field(0x78U, 8, "COUNT", std::byte{' '}));
    EXPECT_TRUE(writer.write_be16(0x96U, count));
    for (std::size_t index = 0; index < capacity; ++index) {
        const auto offset = 0x120U + index * 0x38U;
        EXPECT_TRUE(writer.write_ascii_field(offset, 16, "Target" + std::to_string(index), std::byte{' '}));
        EXPECT_TRUE(writer.write_u8(offset + 0x14U, 0x10U));
    }
    EXPECT_TRUE(writer.write_u8(0x110U, 71U));
    if (version == 4U) {
        const auto tail = size - 0xb0U;
        EXPECT_TRUE(writer.write_ascii_field(tail, 16, "Not a target", std::byte{' '}));
        EXPECT_TRUE(writer.write_u8(tail + 0x78U, 74U));
    }
    return bytes;
}

} // namespace

TEST(ProgCodec, ReadsStoredCountNotCapacityOrTerminalBytes) {
    for (const std::uint32_t version : {1U, 2U, 4U}) {
        for (const auto count : std::array<std::uint16_t, 8>{0, 1, 2, 3, 8, 9, 16, 999}) {
            SCOPED_TRACE(std::to_string(version) + ":" + std::to_string(count));
            const auto capacity = std::max<std::size_t>(8U, count);
            auto bytes = program_bytes(version, count, capacity);
            bytes.resize(bytes.size() + 512U, std::byte{0xa5});
            const auto decoded = axk::decode_object(bytes);
            ASSERT_TRUE(decoded) << decoded.error().message;
            const auto &program = std::get<axk::CurrentProg>(decoded->payload);
            EXPECT_EQ(program.assignments.size(), count);
            EXPECT_EQ(program.control_records[0].device, version == 4U ? 74U : 71U);
            EXPECT_EQ(program.layout.stored_assignment_count, count);
            EXPECT_EQ(program.layout.assignment_capacity, capacity);
            EXPECT_EQ(program.layout.logical_size, bytes.size() - 512U);
            EXPECT_EQ(program.layout.parameter_tail_offset.has_value(), version == 4U);
            EXPECT_EQ(program.effect_blocks.size(), version == 4U ? 6U : 3U);
        }
    }
}

TEST(ProgCodec, PreservesCountedGapsUnknownKindsExactNamesAndPhysicalEffects) {
    for (const std::uint32_t version : {1U, 2U, 4U}) {
        auto bytes = program_bytes(version, 3U, 9U);
        axk::ByteWriter writer{bytes};
        ASSERT_TRUE(writer.write_ascii_field(0x120U, 16U, "1234567890ABCDEF", std::byte{}));
        ASSERT_TRUE(writer.write_ascii_field(0x158U, 16U, "", std::byte{}));
        ASSERT_TRUE(writer.write_u8(0x159U, 'X'));
        ASSERT_TRUE(writer.write_u8(0x1a4U, 0x7fU));
        const auto effect_count = version == 4U ? 6U : 3U;
        for (std::size_t index = 0; index < effect_count; ++index) {
            const auto offset = index < 3U ? 0x98U + index * 0x28U : 0x318U + (index - 3U) * 0x28U;
            ASSERT_TRUE(writer.write_u8(offset + 6U, 97U));
            ASSERT_TRUE(writer.write_u8(offset + 7U, 52U));
            ASSERT_TRUE(writer.write_be16(offset + 8U, static_cast<std::uint16_t>(14800U + index)));
        }
        const auto decoded = axk::decode_object(bytes);
        ASSERT_TRUE(decoded) << decoded.error().message;
        const auto &program = std::get<axk::CurrentProg>(decoded->payload);
        ASSERT_EQ(program.assignments.size(), 3U);
        EXPECT_EQ(program.assignments[0].name, "1234567890ABCDEF");
        EXPECT_EQ(program.assignments[0].raw_handle, 0U);
        EXPECT_TRUE(program.assignments[1].name.empty());
        EXPECT_EQ(program.assignments[2].kind, 0x7fU);
        EXPECT_EQ(program.assignments[2].offset, 0x190U);
        for (std::size_t index = 0; index < effect_count; ++index) {
            EXPECT_EQ(program.effect_blocks[index].type, version == 4U ? 97U : version == 1U ? 47U : 52U);
            EXPECT_EQ(program.effect_blocks[index].parameter_values[0], 14800U + index);
            EXPECT_EQ(program.effect_blocks[index].raw_bytes[7], std::byte{52});
        }
    }
}

TEST(ProgCodec, AcceptsSmallStoredCapacitiesAndNormalizesOnlyLegacyTypes) {
    for (const std::uint32_t version : {1U, 2U, 4U}) {
        for (const auto count : std::array<std::uint16_t, 2>{0, 1}) {
            const auto decoded = axk::decode_object(program_bytes(version, count, count));
            ASSERT_TRUE(decoded);
            EXPECT_EQ(std::get<axk::CurrentProg>(decoded->payload).layout.assignment_capacity, count);
        }
    }
    for (const auto type : std::array<std::uint8_t, 7>{46, 47, 48, 49, 50, 51, 52}) {
        auto bytes = program_bytes(1U, 0U, 0U);
        bytes[0x9fU] = static_cast<std::byte>(type);
        const auto decoded = axk::decode_object(bytes);
        ASSERT_TRUE(decoded);
        const auto &effect = std::get<axk::CurrentProg>(decoded->payload).effect_blocks.front();
        EXPECT_EQ(effect.type, type == 46U ? 46U : type == 52U ? 47U : 0U);
        EXPECT_EQ(effect.raw_bytes[7], static_cast<std::byte>(type));
    }
}

TEST(ProgCodec, GraphRelocationAndCleanupLeaveCapacityTailAndInactiveRowsUntouched) {
    for (const std::uint32_t version : {1U, 2U, 4U}) {
        auto bytes = program_bytes(version, 3U, 9U);
        axk::ByteWriter writer{bytes};
        ASSERT_TRUE(writer.write_be32(0x130U, 0x12345678U));
        ASSERT_TRUE(writer.write_u8(0x158U, 0U));
        ASSERT_TRUE(writer.write_be32(0x168U, 0xabcdef01U));
        bytes.resize(bytes.size() + 32U, std::byte{0xa5});
        const auto decoded = axk::decode_object(bytes);
        ASSERT_TRUE(decoded);
        axk::ObjectCatalog catalog;
        catalog.objects.emplace_back("program", axk::PartitionIndex{0}, axk::SfsId{1}, "partition:0", *decoded);
        const auto graph = axk::build_relationship_graph(catalog);
        ASSERT_EQ(graph.relationships.size(), 2U);
        EXPECT_EQ(graph.relationships[0].assignment_index, 0U);
        EXPECT_EQ(graph.relationships[1].assignment_index, 2U);
        const auto profile = axk::package_internal::build_relocation_profile(*decoded, bytes);
        ASSERT_TRUE(profile) << profile.error().message;
        ASSERT_EQ(profile->relocations.size(), 2U);
        EXPECT_EQ(profile->relocations[0].offset, 0x130U);
        EXPECT_EQ(profile->relocations[1].offset, 0x1a0U);
        auto expected = bytes;
        std::fill_n(expected.begin() + 0x130, 4U, std::byte{});
        EXPECT_EQ(profile->normalized_payload, expected);
        const auto cleaned =
            axk::package_internal::clear_program_assignment_rows(bytes, std::array<std::uint32_t, 1>{2U});
        ASSERT_TRUE(cleaned) << cleaned.error().message;
        expected = bytes;
        std::fill_n(expected.begin() + 0x190, 0x38U, std::byte{});
        EXPECT_EQ(*cleaned, expected);
        const auto after = axk::decode_object(*cleaned);
        ASSERT_TRUE(after);
        EXPECT_EQ(std::get<axk::CurrentProg>(after->payload).assignments.size(), 3U);
        const auto before_failure = bytes;
        EXPECT_FALSE(axk::package_internal::clear_program_assignment_rows(bytes, std::array<std::uint32_t, 2>{0U, 3U}));
        EXPECT_EQ(bytes, before_failure);
    }
    auto bytes = program_bytes(4U, 999U, 999U);
    const auto cleaned =
        axk::package_internal::clear_program_assignment_rows(bytes, std::array<std::uint32_t, 1>{998U});
    ASSERT_TRUE(cleaned);
    auto expected = bytes;
    std::fill_n(expected.begin() + 0x120 + 998 * 0x38, 0x38U, std::byte{});
    EXPECT_EQ(*cleaned, expected);
}

TEST(ProgCodec, FreshWritesActualCountCompleteTailAndNeutralDefaults) {
    for (const auto count : std::array<std::size_t, 5>{1, 3, 8, 9, 16}) {
        axk::ProgramSpec spec;
        spec.number = 1U;
        spec.name = "PROG";
        for (std::size_t index = 0; index < count; ++index)
            spec.assignments.push_back({"SBNK", "Target" + std::to_string(index), 0U, axk::ProgramReceiveMode::sample});
        const auto bytes = axk::detail::prepare_prog_payload(spec);
        ASSERT_TRUE(bytes) << bytes.error().message;
        const auto capacity = std::max<std::size_t>(8U, count);
        const auto size = 0x1d0U + capacity * 0x38U;
        ASSERT_EQ(bytes->size(), size);
        const axk::ByteReader reader{*bytes};
        EXPECT_EQ(*reader.be16(0x96U), static_cast<std::uint16_t>(count));
        EXPECT_EQ(*reader.be32(0x18U), static_cast<std::uint32_t>(size - 0xe0U));
        EXPECT_EQ(*reader.be32(0x1cU), static_cast<std::uint32_t>(size - 0x30U));
        EXPECT_EQ(*reader.be32(0x80U), 0x0005ffffU);
        EXPECT_EQ(*reader.be32(0x84U), 0x00000001U);
        EXPECT_EQ(*reader.be32(0x88U), 0x4000407fU);
        EXPECT_EQ(*reader.be32(0x8cU), 0x000000feU);
        EXPECT_EQ(*reader.be32(0x90U), 0x005a5a27U);
        EXPECT_EQ(*reader.be16(0x94U), std::uint16_t{0x78ff});
        EXPECT_EQ(*reader.ascii_field(0x6cU, 3U), "PRO");
        for (std::size_t index = 0; index < capacity; ++index) {
            const auto row = 0x120U + index * 0x38U;
            EXPECT_EQ(*reader.be32(row + 0x10U), 0U);
            EXPECT_EQ(*reader.u8(row + 0x15U), 0xffU);
            EXPECT_EQ(*reader.u8(row + 0x1eU), 127U);
            EXPECT_EQ(*reader.u8(row + 0x21U), 127U);
            EXPECT_EQ(*reader.u8(row + 0x33U), 1U);
            for (const auto relative : {0x1dU, 0x23U, 0x24U, 0x28U, 0x2dU, 0x30U})
                EXPECT_EQ(*reader.u8(row + relative), std::uint8_t{0xff});
            if (index < count)
                EXPECT_EQ(*reader.decoded_ascii_field(row, 16U), "Target" + std::to_string(index));
            else
                EXPECT_TRUE(*reader.u8(row) == 0U);
        }
        const auto tail = size - 0xb0U;
        for (std::size_t index = 0; index < 6U; ++index) {
            const auto offset = index < 3U ? 0x98U + index * 0x28U : tail + (index - 3U) * 0x28U;
            EXPECT_EQ(*reader.be32(offset), 0x017f7f00U);
            for (std::size_t parameter = 0; parameter < 16U; ++parameter)
                EXPECT_EQ(*reader.be16(offset + 8U + parameter * 2U), 0U);
        }
        EXPECT_EQ(*reader.be32(tail + 0x78U), 0x5b080120U);
        EXPECT_EQ(*reader.be32(tail + 0x7cU), 0x5d1a0120U);
        EXPECT_EQ(*reader.be32(tail + 0x80U), 0x5e2c0120U);
        EXPECT_EQ(*reader.be32(tail + 0x84U), 0U);
        EXPECT_TRUE(std::ranges::equal(*reader.slice(0x110U, 16U), *reader.slice(tail + 0x78U, 16U)));
        EXPECT_EQ(*reader.be16(tail + 0x88U), 0xffffU);
        EXPECT_EQ(*reader.be32(tail + 0x8aU), 1U);
        EXPECT_EQ(*reader.be32(tail + 0x8eU), 0x40004000U);
        EXPECT_EQ(*reader.be32(tail + 0x92U), 0x01400040U);
        EXPECT_TRUE(
            std::ranges::all_of(*reader.slice(tail + 0x96U, 16U), [](std::byte b) { return b == std::byte{64}; }));
        EXPECT_EQ(*reader.u8(tail + 0xa6U), 4U);
        EXPECT_TRUE(std::ranges::all_of(*reader.slice(tail + 0xa7U, 9U), [](std::byte b) { return b == std::byte{}; }));
    }
    axk::ProgramSpec invalid;
    invalid.number = 1U;
    invalid.name = "EMPTY";
    EXPECT_FALSE(axk::detail::prepare_prog_payload(invalid));
    invalid.assignments.resize(17U, {"SBNK", "Target", 0U, axk::ProgramReceiveMode::sample});
    EXPECT_FALSE(axk::detail::prepare_prog_payload(invalid));
}

TEST(ProgCodec, RejectsMalformedCountsAndExtents) {
    for (const std::uint32_t version : {1U, 2U, 4U}) {
        EXPECT_FALSE(axk::decode_object(program_bytes(version, 9U, 8U)));
        EXPECT_FALSE(axk::decode_object(program_bytes(version, 1000U, 1000U)));
        auto truncated = program_bytes(version, 1U, 8U);
        truncated.pop_back();
        EXPECT_FALSE(axk::decode_object(truncated));
        auto conflict = program_bytes(version, 1U, 8U);
        ASSERT_TRUE(axk::ByteWriter{conflict}.write_be32(0x18U, 0U));
        EXPECT_FALSE(axk::decode_object(conflict));
        auto fractional = program_bytes(version, 1U, 8U);
        fractional.push_back(std::byte{});
        ASSERT_TRUE(axk::ByteWriter{fractional}.write_be32(
            0x18U, static_cast<std::uint32_t>(fractional.size() - (version == 4U ? 0xe0U : 0x30U))));
        ASSERT_TRUE(
            axk::ByteWriter{fractional}.write_be32(0x1cU, static_cast<std::uint32_t>(fractional.size() - 0x30U)));
        EXPECT_FALSE(axk::decode_object(fractional));
    }
    for (const std::uint32_t selector : {0U, 3U, 5U, 0xffffffffU}) {
        auto bytes = program_bytes(4U, 1U, 8U);
        ASSERT_TRUE(axk::ByteWriter{bytes}.write_be32(0x14U, selector));
        EXPECT_FALSE(axk::decode_object(bytes));
    }
}
