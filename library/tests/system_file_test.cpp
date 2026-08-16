#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/system_file.hpp"

namespace {

constexpr std::size_t a3000_system_body_size = 0x400U;
constexpr std::size_t system2_body_size = 0x1000U;

std::vector<std::byte> a3000_system_fixture(std::uint8_t basic_receive_channel, std::uint8_t receive_flags) {
    std::vector<std::byte> bytes(a3000_system_body_size);
    for (std::size_t index = 0U; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::byte>(index & 0xffU);
    bytes[0] = std::byte{0x21};
    bytes[1] = std::byte{0x52};
    bytes[2] = std::byte{0x05};
    bytes[3] = std::byte{0x31};
    bytes[0x0e] = std::byte{};
    bytes[0x34] = static_cast<std::byte>(basic_receive_channel);
    bytes[0x36] = static_cast<std::byte>(receive_flags);
    return bytes;
}

std::vector<std::byte> system2_fixture(std::uint8_t model, std::uint8_t mode, std::uint8_t basic_receive_channel,
                                       std::uint8_t receive_flags = 0U) {
    std::vector<std::byte> bytes(system2_body_size);
    bytes[0] = std::byte{0xde};
    bytes[1] = std::byte{0xad};
    bytes[2] = std::byte{0xfa};
    bytes[3] = std::byte{0xce};
    bytes[0x0e] = static_cast<std::byte>(model);
    bytes[0x34] = static_cast<std::byte>(basic_receive_channel);
    bytes[0x36] = static_cast<std::byte>(receive_flags);
    bytes[0x5e] = static_cast<std::byte>(mode);
    for (std::size_t index = 0; index < 32U; ++index)
        bytes[0x60U + index] = static_cast<std::byte>(128U - index);
    return bytes;
}

std::vector<std::byte> wrapped_prf3_record(std::span<const std::byte> inner) {
    std::vector<std::byte> bytes(axk::current_record_envelope_size + inner.size());
    axk::ByteWriter writer{bytes};
    EXPECT_TRUE(writer.write_ascii_field(0U, 12U, "FSFSDEV3SPLX", std::byte{}));
    EXPECT_TRUE(writer.write_ascii_field(0x0cU, 4U, "PRF3", std::byte{}));
    EXPECT_TRUE(writer.write_be32(0x14U, 4U));
    EXPECT_TRUE(writer.write_be32(0x18U, 0x36U));
    EXPECT_TRUE(writer.write_be32(0x1cU, static_cast<std::uint32_t>(inner.size() + 8U)));
    std::ranges::copy(inner, bytes.begin() + static_cast<std::ptrdiff_t>(axk::current_record_envelope_size));
    return bytes;
}

std::vector<std::byte> a3000_system_record_fixture(std::uint8_t basic_receive_channel, std::uint8_t receive_flags) {
    return wrapped_prf3_record(a3000_system_fixture(basic_receive_channel, receive_flags));
}

std::vector<std::byte> system2_record_fixture(std::uint8_t model, std::uint8_t mode, std::uint8_t basic_receive_channel,
                                              std::uint8_t receive_flags = 0U) {
    return wrapped_prf3_record(system2_fixture(model, mode, basic_receive_channel, receive_flags));
}

std::vector<std::byte> copy_section(std::span<const std::byte> bytes, std::size_t offset, std::size_t size) {
    return {bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + size)};
}

axk::IndexRecord directory_record(std::uint32_t sfs_id, std::uint32_t directory_id, std::uint32_t parent_directory_id,
                                  std::vector<axk::DirectoryEntry> entries) {
    axk::IndexRecord record{};
    record.sfs_id = axk::SfsId{sfs_id};
    record.payload_kind = axk::PayloadKind::directory;
    record.directory_id = axk::LinkId{directory_id};
    record.parent_directory_id = axk::LinkId{parent_directory_id};
    record.directory_entries = std::move(entries);
    return record;
}

axk::DirectoryEntry entry(std::string name, std::uint32_t target) {
    return {.link_id = axk::LinkId{target}, .name = std::move(name)};
}

axk::IndexRecord file_record(std::uint32_t sfs_id) {
    axk::IndexRecord record{};
    record.sfs_id = axk::SfsId{sfs_id};
    record.payload_kind = axk::PayloadKind::unknown;
    return record;
}

TEST(SystemFileTest, DecodesA3000ReceiveContextAndPreservesEveryFileSection) {
    const auto bytes = a3000_system_record_fixture(15U, 0xa5U);

    const auto decoded = axk::decode_system_file(axk::SystemFileKind::a3000_system, bytes);

    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->kind, axk::SystemFileKind::a3000_system);
    EXPECT_EQ(decoded->model, axk::ASeriesModel::a3000);
    EXPECT_TRUE(std::ranges::equal(decoded->record_envelope.raw_bytes,
                                   copy_section(bytes, 0U, axk::current_record_envelope_size)));
    EXPECT_EQ(decoded->system_header_bytes, copy_section(bytes, 0x30U, 0x20U));
    EXPECT_EQ(decoded->system_bulk_bytes, copy_section(bytes, 0x50U, 0x348U));
    EXPECT_EQ(decoded->reserved_tail_bytes, copy_section(bytes, 0x398U, 0x98U));

    const auto *context = std::get_if<axk::A3000SystemContext>(&decoded->context);
    ASSERT_NE(context, nullptr);
    EXPECT_EQ(context->basic_receive, (axk::SystemMidiAddress{.port = axk::MidiPort::a, .channel = 16U}));
    EXPECT_TRUE(context->omni);
    EXPECT_FALSE(context->program_change_enabled);
}

TEST(SystemFileTest, DecodesA5000ProgramModeReceiveContextAndAllThirtyTwoParts) {
    const auto bytes = system2_record_fixture(1U, 1U, 18U, 0x02U);

    const auto decoded = axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, bytes);

    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->kind, axk::SystemFileKind::a4000_a5000_system2);
    EXPECT_EQ(decoded->model, axk::ASeriesModel::a5000);
    EXPECT_TRUE(std::ranges::equal(decoded->record_envelope.raw_bytes,
                                   copy_section(bytes, 0U, axk::current_record_envelope_size)));
    EXPECT_EQ(decoded->system_header_bytes, copy_section(bytes, 0x30U, 0x20U));
    EXPECT_EQ(decoded->system_bulk_bytes, copy_section(bytes, 0x50U, 0xfe0U));
    EXPECT_TRUE(decoded->reserved_tail_bytes.empty());

    const auto *context = std::get_if<axk::A4000A5000SystemContext>(&decoded->context);
    ASSERT_NE(context, nullptr);
    EXPECT_EQ(context->saved_program_mode, axk::ProgramMode::multi);
    EXPECT_EQ(context->basic_receive.port, axk::MidiPort::b);
    EXPECT_EQ(context->basic_receive.channel, 3U);
    EXPECT_FALSE(context->omni);
    EXPECT_TRUE(context->program_change_enabled);
    ASSERT_EQ(context->parts.size(), 32U);
    EXPECT_EQ(context->parts[0].part_number, 1U);
    EXPECT_EQ(context->parts[0].midi, (axk::SystemMidiAddress{.port = axk::MidiPort::a, .channel = 1U}));
    EXPECT_EQ(context->parts[0].program_number, 128U);
    EXPECT_FALSE(context->parts[0].master);
    EXPECT_EQ(context->parts[18].part_number, 19U);
    EXPECT_EQ(context->parts[18].midi, (axk::SystemMidiAddress{.port = axk::MidiPort::b, .channel = 3U}));
    EXPECT_EQ(context->parts[18].program_number, 110U);
    EXPECT_TRUE(context->parts[18].master);
}

TEST(SystemFileTest, DecodesSamplerAuthoredWrappedA5000MultiContext) {
    auto inner = system2_fixture(1U, 1U, 0U, 0U);
    // These assignment bytes were captured from a sampler-authored A5000 SYSTEM2 file. The test helper synthesizes
    // only the surrounding record and unrelated fields so the regression remains small and deterministic.
    constexpr std::array<std::uint8_t, 32> stored_programs{
        1U, 7U, 2U, 6U, 3U, 5U, 4U, 1U, 17U, 11U, 1U, 1U, 1U, 1U, 1U, 1U,
        2U, 2U, 2U, 2U, 2U, 2U, 2U, 2U, 2U,  2U,  2U, 2U, 2U, 2U, 2U, 2U,
    };
    for (std::size_t index = 0U; index < stored_programs.size(); ++index)
        inner[0x60U + index] = static_cast<std::byte>(stored_programs[index]);
    const auto bytes = wrapped_prf3_record(inner);

    const auto decoded = axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, bytes);

    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->model, axk::ASeriesModel::a5000);
    const auto *context = std::get_if<axk::A4000A5000SystemContext>(&decoded->context);
    ASSERT_NE(context, nullptr);
    EXPECT_EQ(context->saved_program_mode, axk::ProgramMode::multi);
    ASSERT_EQ(context->parts.size(), 32U);
    EXPECT_TRUE(context->parts.front().master);
    EXPECT_EQ(context->parts[0].program_number, 1U);
    EXPECT_EQ(context->parts[1].program_number, 7U);
    EXPECT_EQ(context->parts[8].program_number, 17U);
    EXPECT_EQ(context->parts[9].program_number, 11U);
    EXPECT_EQ(context->parts[15].program_number, 1U);
    EXPECT_EQ(context->parts[16].program_number, 2U);
    EXPECT_EQ(context->parts.back().program_number, 2U);
}

TEST(SystemFileTest, DecodesOnlyTheSixteenA4000Parts) {
    const auto bytes = system2_record_fixture(0U, 0U, 15U);

    const auto decoded = axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, bytes);

    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->model, axk::ASeriesModel::a4000);
    const auto *context = std::get_if<axk::A4000A5000SystemContext>(&decoded->context);
    ASSERT_NE(context, nullptr);
    EXPECT_EQ(context->saved_program_mode, axk::ProgramMode::single);
    ASSERT_EQ(context->parts.size(), 16U);
    EXPECT_TRUE(context->parts.back().master);
}

TEST(SystemFileTest, RejectsMalformedOrUnsupportedA3000SystemFiles) {
    auto truncated = a3000_system_record_fixture(0U, 0U);
    truncated.pop_back();
    EXPECT_FALSE(axk::decode_system_file(axk::SystemFileKind::a3000_system, truncated));

    auto wrong_magic = a3000_system_record_fixture(0U, 0U);
    wrong_magic[0] = std::byte{};
    EXPECT_FALSE(axk::decode_system_file(axk::SystemFileKind::a3000_system, wrong_magic));

    auto wrong_model = a3000_system_record_fixture(0U, 0U);
    wrong_model[axk::current_record_envelope_size + 0x0eU] = std::byte{1U};
    EXPECT_FALSE(axk::decode_system_file(axk::SystemFileKind::a3000_system, wrong_model));

    EXPECT_FALSE(axk::decode_system_file(axk::SystemFileKind::a3000_system, a3000_system_record_fixture(16U, 0U)));
}

TEST(SystemFileTest, RejectsMalformedOrUnsupportedSystem2Files) {
    auto truncated = system2_record_fixture(1U, 1U, 0U);
    truncated.pop_back();
    EXPECT_FALSE(axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, truncated));

    auto wrong_magic = system2_record_fixture(1U, 1U, 0U);
    wrong_magic[0] = std::byte{};
    EXPECT_FALSE(axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, wrong_magic));

    EXPECT_FALSE(axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, system2_record_fixture(2U, 1U, 0U)));
    EXPECT_FALSE(axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, system2_record_fixture(1U, 2U, 0U)));
    EXPECT_FALSE(
        axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, system2_record_fixture(0U, 1U, 16U)));

    auto zero_program = system2_record_fixture(1U, 1U, 0U);
    zero_program[axk::current_record_envelope_size + 0x60U] = std::byte{0U};
    EXPECT_FALSE(axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, zero_program));

    auto excessive_program = system2_record_fixture(1U, 1U, 0U);
    excessive_program[axk::current_record_envelope_size + 0x60U] = std::byte{129U};
    EXPECT_FALSE(axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, excessive_program));
}

TEST(SystemFileTest, LocatesBothPartitionLevelSystemFileKindsIndependently) {
    axk::Partition partition{};
    partition.index = axk::PartitionIndex{2U};
    partition.records = {
        directory_record(1U, 1U, 1U, {entry(".", 1U), entry("..", 1U), entry("PRF3", 4U), entry("Volume", 5U)}),
        directory_record(4U, 4U, 1U, {entry(".", 4U), entry("..", 1U), entry("SYSTEM", 8U), entry("SYSTEM2", 9U)}),
        file_record(8U),
        file_record(9U),
    };

    const auto system = axk::locate_system_file_record(partition, axk::SystemFileKind::a3000_system);
    const auto system2 = axk::locate_system_file_record(partition, axk::SystemFileKind::a4000_a5000_system2);

    ASSERT_TRUE(system) << system.error().message;
    ASSERT_TRUE(*system);
    EXPECT_EQ(**system, axk::SfsId{8U});
    ASSERT_TRUE(system2) << system2.error().message;
    ASSERT_TRUE(*system2);
    EXPECT_EQ(**system2, axk::SfsId{9U});
}

TEST(SystemFileTest, UsesTheSharedSfsRootAndReservedEntryContract) {
    axk::Partition partition{};
    partition.index = axk::PartitionIndex{3U};
    partition.records = {
        directory_record(7U, 7U, 7U, {entry(".", 7U), entry("..", 7U), entry("PRF3", 8U)}),
        directory_record(8U, 8U, 7U, {entry(".", 8U), entry("..", 7U)}),
    };

    const auto root = axk::locate_partition_root_record(partition);

    ASSERT_TRUE(root) << root.error().message;
    EXPECT_EQ(*root, axk::SfsId{7U});
    EXPECT_TRUE(axk::is_partition_support_root_entry("PRF3"));
    EXPECT_TRUE(axk::is_partition_support_root_entry("PRF3   "));
    EXPECT_TRUE(axk::is_partition_support_root_entry(std::string_view{"PRF3\0ignored", 12U}));
    EXPECT_FALSE(axk::is_partition_support_root_entry("prf3"));
    EXPECT_FALSE(axk::is_partition_support_root_entry("Volume"));
}

TEST(SystemFileTest, DoesNotTreatAVolumePrf3CategoryAsThePartitionSystemDirectory) {
    axk::Partition partition{};
    partition.records = {
        directory_record(1U, 1U, 1U, {entry(".", 1U), entry("..", 1U), entry("Volume", 5U)}),
        directory_record(5U, 5U, 1U, {entry(".", 5U), entry("..", 1U), entry("PRF3", 6U)}),
        directory_record(6U, 6U, 5U, {entry(".", 6U), entry("..", 5U), entry("SYSTEM", 8U)}),
        file_record(8U),
    };

    const auto located = axk::locate_system_file_record(partition, axk::SystemFileKind::a3000_system);

    ASSERT_TRUE(located) << located.error().message;
    EXPECT_EQ(*located, std::nullopt);
}

TEST(SystemFileTest, KeepsAValidSiblingUsableWhenTheOtherFilenameIsAmbiguous) {
    axk::Partition partition{};
    partition.records = {
        directory_record(1U, 1U, 1U, {entry(".", 1U), entry("..", 1U), entry("PRF3", 4U)}),
        directory_record(
            4U, 4U, 1U,
            {entry(".", 4U), entry("..", 1U), entry("SYSTEM", 8U), entry("SYSTEM", 10U), entry("SYSTEM2", 9U)}),
        file_record(8U),
        file_record(9U),
        file_record(10U),
    };

    EXPECT_FALSE(axk::locate_system_file_record(partition, axk::SystemFileKind::a3000_system));
    const auto system2 = axk::locate_system_file_record(partition, axk::SystemFileKind::a4000_a5000_system2);
    ASSERT_TRUE(system2) << system2.error().message;
    ASSERT_TRUE(*system2);
    EXPECT_EQ(**system2, axk::SfsId{9U});
}

TEST(SystemFileTest, ReportsAnAbsentSystemFileWithoutGuessing) {
    axk::Partition partition{};
    partition.records = {
        directory_record(1U, 1U, 1U, {entry(".", 1U), entry("..", 1U), entry("Volume", 5U)}),
    };

    const auto located = axk::locate_system_file_record(partition, axk::SystemFileKind::a3000_system);

    ASSERT_TRUE(located) << located.error().message;
    EXPECT_EQ(*located, std::nullopt);
}

TEST(SystemFileTest, RejectsAmbiguousOrDanglingSystemFilePaths) {
    axk::Partition ambiguous{};
    ambiguous.records = {
        directory_record(1U, 1U, 1U, {entry(".", 1U), entry("..", 1U), entry("PRF3", 4U), entry("PRF3", 6U)}),
        directory_record(4U, 4U, 1U, {entry(".", 4U), entry("..", 1U)}),
        directory_record(6U, 6U, 1U, {entry(".", 6U), entry("..", 1U)}),
    };
    EXPECT_FALSE(axk::locate_system_file_record(ambiguous, axk::SystemFileKind::a3000_system));

    axk::Partition dangling{};
    dangling.records = {
        directory_record(1U, 1U, 1U, {entry(".", 1U), entry("..", 1U), entry("PRF3", 4U)}),
        directory_record(4U, 4U, 1U, {entry(".", 4U), entry("..", 1U), entry("SYSTEM2", 9U)}),
    };
    EXPECT_FALSE(axk::locate_system_file_record(dangling, axk::SystemFileKind::a4000_a5000_system2));
}

} // namespace
