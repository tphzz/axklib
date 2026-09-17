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
#include "axklib/effects.hpp"
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

std::vector<std::byte> system2_fixture(std::uint8_t revision, std::uint8_t mode, std::uint8_t basic_receive_channel,
                                       std::uint8_t receive_flags = 0U) {
    std::vector<std::byte> bytes(system2_body_size);
    bytes[0] = std::byte{0xde};
    bytes[1] = std::byte{0xad};
    bytes[2] = std::byte{0xfa};
    bytes[3] = std::byte{0xce};
    bytes[0x0e] = static_cast<std::byte>(revision);
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

std::vector<std::byte> system2_record_fixture(std::uint8_t revision, std::uint8_t mode,
                                              std::uint8_t basic_receive_channel, std::uint8_t receive_flags = 0U) {
    return wrapped_prf3_record(system2_fixture(revision, mode, basic_receive_channel, receive_flags));
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
    return {.raw_link_id = axk::LinkId{target}, .target_link_id = axk::LinkId{target}, .name = std::move(name)};
}

axk::IndexRecord file_record(std::uint32_t sfs_id) {
    axk::IndexRecord record{};
    record.sfs_id = axk::SfsId{sfs_id};
    record.payload_kind = axk::PayloadKind::unknown;
    return record;
}

TEST(SystemFileTest, GlobalProjectionPreservesWrappedRecordsAndAgreesWithRoutingContext) {
    for (const auto kind : {axk::SystemFileKind::a3000_system, axk::SystemFileKind::a4000_a5000_system2}) {
        const auto native = kind == axk::SystemFileKind::a3000_system;
        const auto payload = native ? a3000_system_record_fixture(15, 0x43) : system2_record_fixture(1, 1, 31, 0x43);
        const auto file = axk::decode_system_file(kind, payload);
        ASSERT_TRUE(file);
        const auto global = axk::decode_system_global(*file);
        ASSERT_TRUE(global);
        EXPECT_EQ(global->parameters.basic_receive_channel_selection, native ? 15 : 31);
        std::visit(
            [&](const auto &context) {
                EXPECT_EQ(global->parameters.omni, context.omni);
                EXPECT_EQ(global->parameters.program_change_enabled, context.program_change_enabled);
            },
            file->context);
        if (!native) {
            const auto &context = std::get<axk::A4000A5000SystemContext>(file->context);
            EXPECT_EQ(global->parameters.program_mode, context.saved_program_mode);
            for (std::size_t part = 0; part < context.parts.size(); ++part) {
                ASSERT_TRUE(global->parameters.part_program_numbers[part]);
                ASSERT_TRUE(context.parts[part].program_number);
                EXPECT_EQ(*global->parameters.part_program_numbers[part], *context.parts[part].program_number);
            }
        }
        const auto encoded = axk::encode_system_file(*file);
        ASSERT_TRUE(encoded);
        EXPECT_EQ(*encoded, payload);
    }
}

TEST(SystemFileTest, DecodesA3000ReceiveContextAndPreservesEveryFileSection) {
    const auto bytes = a3000_system_record_fixture(15U, 0xa5U);

    const auto decoded = axk::decode_system_file(axk::SystemFileKind::a3000_system, bytes);

    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->kind, axk::SystemFileKind::a3000_system);
    EXPECT_EQ(decoded->storage_revision, 0U);
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

TEST(SystemFileTest, DecodesRegisteredProgramBlocksWithoutUsingAssignmentOrPaddingBytes) {
    for (const auto revision : {0U, 1U}) {
        auto body = system2_fixture(static_cast<std::uint8_t>(revision), 0U, 0U);
        body[0x600] = std::byte{0xfa};
        body[0x62c] = std::byte{0x91};
        body[0x637] = std::byte{101};
        body[0x63a] = std::byte{0xf4};
        body[0x5ed] = std::byte{70};
        body[0x565] = std::byte{3};
        body[0x5fc] = std::byte{0x80};
        body[0x60a] = std::byte{88};
        body[0x61a] = std::byte{0x1e};
        body[0x4ec] = std::byte{1};
        body[0x4f2] = std::byte{1};
        body[0x5c4] = std::byte{1};
        body[0x5ca] = std::byte{1};
        std::fill(body.begin() + 0x624, body.begin() + 0x62c, std::byte{0xee});
        const auto decoded =
            axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, wrapped_prf3_record(body));
        ASSERT_TRUE(decoded);
        const auto original = decoded->system_bulk_bytes;
        const auto parameters = axk::decode_system_registered_program(*decoded);
        ASSERT_TRUE(parameters) << parameters.error().message;
        EXPECT_EQ(parameters->level, 101U);
        EXPECT_EQ(parameters->transpose, -12);
        EXPECT_EQ(parameters->lfo.sync, 2U);
        EXPECT_EQ(parameters->effect_connections[0], 2U);
        EXPECT_EQ(parameters->effect_connections[1], 2U);
        EXPECT_EQ(parameters->controllers[0].function, 70U);
        EXPECT_EQ(parameters->controller_reset.b[15], true);
        EXPECT_EQ(parameters->step_wave.values[0], 88U);
        EXPECT_EQ(parameters->step_wave.step_count, 16U);
        EXPECT_EQ(parameters->step_wave.slope, axk::ProgramStepWaveSlope::both);
        EXPECT_EQ(parameters->effects[0].type, 1U);
        EXPECT_EQ(parameters->effects[0].enabled, true);
        EXPECT_EQ(parameters->effects[5].type, 1U);
        EXPECT_EQ(parameters->effects[5].enabled, true);
        EXPECT_EQ(decoded->system_bulk_bytes, original);
    }
}

TEST(SystemFileTest, RejectsTruncatedOrUnsupportedRegisteredProgramStorage) {
    auto decoded =
        axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, system2_record_fixture(1U, 0U, 0U));
    ASSERT_TRUE(decoded);
    decoded->system_bulk_bytes.resize(0x621U);
    EXPECT_FALSE(axk::decode_system_registered_program(*decoded));
    decoded->kind = axk::SystemFileKind::a3000_system;
    EXPECT_FALSE(axk::decode_system_registered_program(*decoded));
}

TEST(SystemFileTest, DecodesA3000RegisteredProgramWithoutCurrentGenerationFields) {
    auto body = a3000_system_fixture(0U, 0U);
    body[0x334] = std::byte{0xdd};
    body[0x335] = std::byte{0xed};
    body[0x336] = std::byte{0x80};
    body[0x337] = std::byte{1};
    body[0x338] = std::byte{};
    body[0x339] = std::byte{2};
    body[0x33a] = std::byte{0xc1};
    body[0x33b] = std::byte{4};
    body[0x33c] = std::byte{127};
    body[0x33d] = std::byte{5};
    body[0x33e] = std::byte{100};
    body[0x33f] = std::byte{101};
    body[0x342] = std::byte{0x81};
    body[0x343] = std::byte{0xfe};
    body[0x344] = std::byte{3};
    body[0x345] = std::byte{1};
    body[0x346] = std::byte{127};
    body[0x347] = std::byte{123};
    body[0x348] = std::byte{250};
    body[0x349] = std::byte{0xff};
    for (std::size_t index = 0; index < 4U; ++index) {
        body[0x31cU + index * 4U] = std::byte{125};
        body[0x31dU + index * 4U] = std::byte{63};
        body[0x31eU + index * 4U] = std::byte{3};
        body[0x31fU + index * 4U] = std::byte{0xc1};
    }
    const auto decoded = axk::decode_system_file(axk::SystemFileKind::a3000_system, wrapped_prf3_record(body));
    ASSERT_TRUE(decoded);
    const auto original = *decoded;
    const auto parameters = axk::decode_system_registered_program(*decoded);
    ASSERT_TRUE(parameters) << parameters.error().message;
    EXPECT_EQ(parameters->level, 101U);
    EXPECT_EQ(parameters->transpose, -127);
    EXPECT_EQ(parameters->controller_reset.a[0], true);
    EXPECT_EQ(parameters->controller_reset.a[15], true);
    EXPECT_EQ(parameters->note_toggle.a[1], true);
    EXPECT_EQ(parameters->lfo.sync, 1U);
    EXPECT_EQ(parameters->lfo.cycle, 5U);
    EXPECT_EQ(parameters->lfo.wave, 5U);
    EXPECT_EQ(parameters->lfo.initial_phase, 3U);
    EXPECT_EQ(parameters->lfo.reset_channel, -2);
    EXPECT_EQ(parameters->lfo.reset_note, -1);
    EXPECT_EQ(parameters->lfo.tempo, 250U);
    EXPECT_EQ(parameters->lfo.sample_hold_speed, 123U);
    EXPECT_EQ(parameters->portamento.type, 3U);
    EXPECT_EQ(parameters->portamento.rate, 1U);
    EXPECT_EQ(parameters->portamento.time, 127U);
    EXPECT_EQ(parameters->ad.enabled, true);
    EXPECT_EQ(parameters->ad.source, 2U);
    EXPECT_EQ(parameters->ad.left.pan, -63);
    EXPECT_EQ(parameters->ad.left.output1.destination, 4U);
    EXPECT_EQ(parameters->ad.left.output1.level, 127U);
    EXPECT_EQ(parameters->ad.left.output2.destination, 5U);
    EXPECT_EQ(parameters->ad.left.output2.level, 100U);
    EXPECT_EQ(parameters->effect_connections[0], 3U);
    for (const auto &controller : parameters->controllers) {
        EXPECT_EQ(controller.device, 125U);
        EXPECT_EQ(controller.function, 63U);
        EXPECT_EQ(controller.type, 3U);
        EXPECT_EQ(controller.range, -63);
    }
    EXPECT_FALSE(parameters->ad.right.pan);
    EXPECT_FALSE(parameters->ad.right.output1.destination);
    EXPECT_FALSE(parameters->ad.right.output2.level);
    EXPECT_FALSE(parameters->controller_reset.b[0]);
    EXPECT_FALSE(parameters->note_toggle.b[15]);
    EXPECT_FALSE(parameters->effect_connections[1]);
    EXPECT_FALSE(parameters->step_wave.step_count);
    EXPECT_FALSE(parameters->step_wave.values[0]);
    for (std::size_t slot = 3; slot < 6; ++slot)
        EXPECT_FALSE(parameters->effects[slot].type);
    EXPECT_EQ(*decoded, original);
}

TEST(SystemFileTest, A3000RegisteredEffectsUseNativeTypeByteAndVisibleWordDomains) {
    for (std::uint8_t type = 0; type <= 54; ++type) {
        auto body = a3000_system_fixture(0U, 0U);
        axk::ByteWriter writer{body};
        const auto info = axk::effect_write_info(type);
        ASSERT_TRUE(info);
        for (std::size_t slot = 0; slot < 3; ++slot) {
            const auto offset = 0x2a4U + slot * 0x28U;
            body[offset] = std::byte{1};
            body[offset + 1] = std::byte{127};
            body[offset + 2] = std::byte{120};
            body[offset + 3] = std::byte{63};
            body[offset + 4] = std::byte{5};
            body[offset + 5] = std::byte{0x82};
            body[offset + 6] = std::byte{96};
            body[offset + 7] = static_cast<std::byte>(type);
            for (std::size_t word = 0; word < 16; ++word) {
                const auto &domain = info->parameters[word];
                ASSERT_LT(domain.maximum, 0xffffU);
                ASSERT_TRUE(writer.write_be16(offset + 8 + word * 2,
                                              domain.kind == axk::EffectParameterKind::stored_value && slot < 2U
                                                  ? (slot == 0U ? domain.minimum : domain.maximum)
                                                  : std::uint16_t{0xffff}));
            }
        }
        const auto decoded = axk::decode_system_file(axk::SystemFileKind::a3000_system, wrapped_prf3_record(body));
        ASSERT_TRUE(decoded);
        const auto parameters = axk::decode_system_registered_program(*decoded);
        ASSERT_TRUE(parameters);
        for (std::size_t slot = 0; slot < 3; ++slot) {
            const auto &effect = parameters->effects[slot];
            EXPECT_EQ(effect.type, type);
            EXPECT_EQ(effect.enabled, true);
            EXPECT_EQ(effect.input_level, 127U);
            EXPECT_EQ(effect.output_level, 120U);
            EXPECT_EQ(effect.pan, 63);
            EXPECT_EQ(effect.width, -126);
            EXPECT_EQ(effect.destination, 5U);
            for (std::size_t word = 0; word < 16; ++word) {
                const auto &domain = info->parameters[word];
                if (domain.kind == axk::EffectParameterKind::stored_value && slot < 2U)
                    EXPECT_EQ(effect.parameters[word], slot == 0U ? domain.minimum : domain.maximum);
                else
                    EXPECT_FALSE(effect.parameters[word]);
            }
        }
    }
}

TEST(SystemFileTest, A3000RegisteredDomainOverflowStaysRawAndSyncIgnoresBitSeven) {
    auto body = a3000_system_fixture(0U, 0U);
    body[0x334] = std::byte{0x80};
    body[0x335] = std::byte{0x30};
    body[0x343] = std::byte{17};
    body[0x33b] = std::byte{5};
    body[0x33d] = std::byte{6};
    body[0x31c] = std::byte{126};
    body[0x31d] = std::byte{64};
    body[0x2a8] = std::byte{6};
    body[0x2ab] = std::byte{55};
    const auto decoded = axk::decode_system_file(axk::SystemFileKind::a3000_system, wrapped_prf3_record(body));
    ASSERT_TRUE(decoded);
    const auto original = *decoded;
    const auto parameters = axk::decode_system_registered_program(*decoded);
    ASSERT_TRUE(parameters);
    EXPECT_EQ(parameters->lfo.sync, 0U);
    EXPECT_FALSE(parameters->lfo.wave);
    EXPECT_FALSE(parameters->lfo.reset_channel);
    EXPECT_FALSE(parameters->ad.left.output1.destination);
    EXPECT_FALSE(parameters->ad.left.output2.destination);
    EXPECT_FALSE(parameters->controllers[0].device);
    EXPECT_FALSE(parameters->controllers[0].function);
    EXPECT_FALSE(parameters->effects[0].destination);
    EXPECT_FALSE(parameters->effects[0].type);
    EXPECT_FALSE(parameters->effects[0].parameters[0]);
    EXPECT_EQ(*decoded, original);
    auto malformed = *decoded;
    malformed.system_bulk_bytes.pop_back();
    EXPECT_FALSE(axk::decode_system_registered_program(malformed));
    malformed = *decoded;
    malformed.storage_revision = 1;
    EXPECT_FALSE(axk::decode_system_registered_program(malformed));
}

TEST(SystemFileTest, KeepsInvalidRegisteredLeavesRawWithoutRejectingTheWholeBlock) {
    auto body = system2_fixture(1U, 0U, 0U);
    body[0x637] = std::byte{0xff};
    body[0x5ed] = std::byte{0xff};
    body[0x4f2] = std::byte{0xff};
    body[0x61a] = std::byte{7};
    body[0x5f0] = std::byte{74};
    const auto decoded = axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, wrapped_prf3_record(body));
    ASSERT_TRUE(decoded);
    const auto parameters = axk::decode_system_registered_program(*decoded);
    ASSERT_TRUE(parameters);
    EXPECT_FALSE(parameters->level);
    EXPECT_FALSE(parameters->controllers[0].function);
    EXPECT_FALSE(parameters->effects[0].type);
    EXPECT_FALSE(parameters->step_wave.step_count);
    EXPECT_EQ(parameters->controllers[1].device, 74U);
    EXPECT_EQ(decoded->system_bulk_bytes[0x617], std::byte{0xff});
}

TEST(SystemFileTest, DecodesRevisionOneProgramModeReceiveContextAndAllThirtyTwoParts) {
    const auto bytes = system2_record_fixture(1U, 1U, 18U, 0x02U);

    const auto decoded = axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, bytes);

    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->kind, axk::SystemFileKind::a4000_a5000_system2);
    EXPECT_EQ(decoded->storage_revision, 1U);
    EXPECT_TRUE(std::ranges::equal(decoded->record_envelope.raw_bytes,
                                   copy_section(bytes, 0U, axk::current_record_envelope_size)));
    EXPECT_EQ(decoded->system_header_bytes, copy_section(bytes, 0x30U, 0x20U));
    EXPECT_EQ(decoded->system_bulk_bytes, copy_section(bytes, 0x50U, 0xfe0U));
    EXPECT_TRUE(decoded->reserved_tail_bytes.empty());

    const auto *context = std::get_if<axk::A4000A5000SystemContext>(&decoded->context);
    ASSERT_NE(context, nullptr);
    EXPECT_EQ(context->saved_program_mode, axk::ProgramMode::multi);
    ASSERT_TRUE(context->basic_receive);
    EXPECT_EQ(context->basic_receive->port, axk::MidiPort::b);
    EXPECT_EQ(context->basic_receive->channel, 3U);
    EXPECT_FALSE(context->omni);
    EXPECT_TRUE(context->program_change_enabled);
    ASSERT_EQ(context->parts.size(), 32U);
    EXPECT_EQ(context->parts[0].part_number, 1U);
    EXPECT_EQ(context->parts[0].midi, (axk::SystemMidiAddress{.port = axk::MidiPort::a, .channel = 1U}));
    EXPECT_EQ(context->parts[0].program_number, 128U);
    EXPECT_EQ(context->parts[0].master, false);
    EXPECT_EQ(context->parts[18].part_number, 19U);
    EXPECT_EQ(context->parts[18].midi, (axk::SystemMidiAddress{.port = axk::MidiPort::b, .channel = 3U}));
    EXPECT_EQ(context->parts[18].program_number, 110U);
    EXPECT_EQ(context->parts[18].master, true);
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
    EXPECT_EQ(decoded->storage_revision, 1U);
    const auto *context = std::get_if<axk::A4000A5000SystemContext>(&decoded->context);
    ASSERT_NE(context, nullptr);
    EXPECT_EQ(context->saved_program_mode, axk::ProgramMode::multi);
    ASSERT_EQ(context->parts.size(), 32U);
    EXPECT_EQ(context->parts.front().master, true);
    EXPECT_EQ(context->parts[0].program_number, 1U);
    EXPECT_EQ(context->parts[1].program_number, 7U);
    EXPECT_EQ(context->parts[8].program_number, 17U);
    EXPECT_EQ(context->parts[9].program_number, 11U);
    EXPECT_EQ(context->parts[15].program_number, 1U);
    EXPECT_EQ(context->parts[16].program_number, 2U);
    EXPECT_EQ(context->parts.back().program_number, 2U);
}

TEST(SystemFileTest, RevisionZeroRetainsAllThirtyTwoPartsWithPortAMaster) {
    const auto bytes = system2_record_fixture(0U, 0U, 15U);

    const auto decoded = axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, bytes);

    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->storage_revision, 0U);
    const auto *context = std::get_if<axk::A4000A5000SystemContext>(&decoded->context);
    ASSERT_NE(context, nullptr);
    EXPECT_EQ(context->saved_program_mode, axk::ProgramMode::single);
    ASSERT_EQ(context->parts.size(), 32U);
    EXPECT_EQ(context->parts[15].master, true);
    EXPECT_EQ(context->parts.back().master, false);
}

TEST(SystemFileTest, RevisionZeroRetainsPortBAndAllStoredAssignments) {
    const auto bytes = system2_record_fixture(0U, 1U, 31U);
    const auto decoded = axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, bytes);
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto &context = std::get<axk::A4000A5000SystemContext>(decoded->context);
    EXPECT_EQ(context.basic_receive, (axk::SystemMidiAddress{.port = axk::MidiPort::b, .channel = 16U}));
    ASSERT_EQ(context.parts.size(), 32U);
    EXPECT_EQ(context.parts.back().program_number, 97U);
    EXPECT_EQ(context.parts.back().master, true);
}

TEST(SystemFileTest, RejectsMalformedOrUnsupportedA3000SystemFiles) {
    auto truncated = a3000_system_record_fixture(0U, 0U);
    truncated.pop_back();
    EXPECT_FALSE(axk::decode_system_file(axk::SystemFileKind::a3000_system, truncated));

    auto wrong_magic = a3000_system_record_fixture(0U, 0U);
    wrong_magic[0] = std::byte{};
    EXPECT_FALSE(axk::decode_system_file(axk::SystemFileKind::a3000_system, wrong_magic));

    auto wrong_revision = a3000_system_record_fixture(0U, 0U);
    wrong_revision[axk::current_record_envelope_size + 0x0eU] = std::byte{1U};
    EXPECT_FALSE(axk::decode_system_file(axk::SystemFileKind::a3000_system, wrong_revision));
}

TEST(SystemFileTest, RejectsMalformedOrUnsupportedSystem2Files) {
    auto truncated = system2_record_fixture(1U, 1U, 0U);
    truncated.pop_back();
    EXPECT_FALSE(axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, truncated));

    auto wrong_magic = system2_record_fixture(1U, 1U, 0U);
    wrong_magic[0] = std::byte{};
    EXPECT_FALSE(axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, wrong_magic));

    EXPECT_FALSE(axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, system2_record_fixture(2U, 1U, 0U)));
}

TEST(SystemFileTest, RetainsOutOfDomainContextBytesWithoutRejectingOtherSettings) {
    for (const auto revision : {0U, 1U}) {
        auto bytes = system2_record_fixture(static_cast<std::uint8_t>(revision), 2U, 255U);
        bytes[0x30U + 0x60U] = std::byte{0};
        bytes[0x30U + 0x61U] = std::byte{129};
        const auto decoded = axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, bytes);
        ASSERT_TRUE(decoded);
        const auto &context = std::get<axk::A4000A5000SystemContext>(decoded->context);
        EXPECT_FALSE(context.basic_receive);
        EXPECT_FALSE(context.saved_program_mode);
        ASSERT_EQ(context.parts.size(), 32U);
        EXPECT_FALSE(context.parts[0].program_number);
        EXPECT_FALSE(context.parts[1].program_number);
        EXPECT_EQ(context.parts[2].program_number, 126U);
        for (const auto &part : context.parts)
            EXPECT_FALSE(part.master.has_value());
        EXPECT_EQ(decoded->system_bulk_bytes, copy_section(bytes, 0x50U, 0xfe0U));
        EXPECT_TRUE(axk::decode_system_recording(*decoded));
        EXPECT_TRUE(axk::decode_system_registered_sample(*decoded));
        EXPECT_TRUE(axk::decode_system_registered_program(*decoded));
    }
    const auto bytes = a3000_system_record_fixture(255U, 0xffU);
    const auto decoded = axk::decode_system_file(axk::SystemFileKind::a3000_system, bytes);
    ASSERT_TRUE(decoded);
    const auto &context = std::get<axk::A3000SystemContext>(decoded->context);
    EXPECT_FALSE(context.basic_receive);
    EXPECT_TRUE(context.omni);
    EXPECT_TRUE(context.program_change_enabled);
    EXPECT_EQ(decoded->system_bulk_bytes, copy_section(bytes, 0x50U, 0x348U));
}

TEST(SystemFileTest, ContextProjectionUsesExactStoredDomainsWithoutNormalizingBytes) {
    for (unsigned raw = 0U; raw <= 255U; ++raw) {
        const auto value = static_cast<std::uint8_t>(raw);
        const auto native_bytes = a3000_system_record_fixture(value, value);
        const auto native = axk::decode_system_file(axk::SystemFileKind::a3000_system, native_bytes);
        ASSERT_TRUE(native);
        const auto &native_context = std::get<axk::A3000SystemContext>(native->context);
        EXPECT_EQ(native_context.basic_receive.has_value(), raw < 16U);
        EXPECT_EQ(native_context.omni, (raw & 1U) != 0U);
        EXPECT_EQ(native_context.program_change_enabled, (raw & 2U) != 0U);
        EXPECT_EQ(axk::encode_system_file(*native).value(), native_bytes);
        for (const auto revision : {0U, 1U}) {
            auto bytes = system2_record_fixture(static_cast<std::uint8_t>(revision), value, value, value);
            bytes[0x90U] = static_cast<std::byte>(value);
            bytes[0xafU] = static_cast<std::byte>(value);
            const auto decoded = axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, bytes);
            ASSERT_TRUE(decoded);
            const auto &context = std::get<axk::A4000A5000SystemContext>(decoded->context);
            EXPECT_EQ(context.basic_receive.has_value(), raw < 32U);
            EXPECT_EQ(context.saved_program_mode.has_value(), raw < 2U);
            ASSERT_EQ(context.parts.size(), 32U);
            for (const auto index : {0U, 31U}) {
                const auto &part = context.parts[index];
                EXPECT_EQ(part.program_number.has_value(), raw >= 1U && raw <= 128U);
                if (part.program_number)
                    EXPECT_EQ(*part.program_number, raw);
                EXPECT_EQ(part.master.has_value(), raw < 32U);
                if (part.master)
                    EXPECT_EQ(*part.master, raw == index);
            }
            EXPECT_EQ(axk::encode_system_file(*decoded).value(), bytes);
        }
    }
}

TEST(SystemFileTest, EncodesEveryRetainedByteIncludingUnknownAndInvalidValues) {
    for (const auto kind : {axk::SystemFileKind::a3000_system, axk::SystemFileKind::a4000_a5000_system2}) {
        for (const auto pattern : {0U, 0x55U, 0xaaU, 0xffU}) {
            for (const auto revision : {0U, 1U}) {
                if (kind == axk::SystemFileKind::a3000_system && revision != 0U)
                    continue;
                auto body = kind == axk::SystemFileKind::a3000_system
                                ? a3000_system_fixture(15U, 3U)
                                : system2_fixture(static_cast<std::uint8_t>(revision), 1U, 31U);
                std::fill(body.begin() + 0x20, body.end(), static_cast<std::byte>(pattern));
                auto bytes = wrapped_prf3_record(body);
                bytes[0x20U] = static_cast<std::byte>(pattern);
                const auto decoded = axk::decode_system_file(kind, bytes);
                ASSERT_TRUE(decoded);
                const auto encoded = axk::encode_system_file(*decoded);
                ASSERT_TRUE(encoded) << encoded.error().message;
                EXPECT_EQ(*encoded, bytes);
                const auto round_trip = axk::decode_system_file(kind, *encoded);
                ASSERT_TRUE(round_trip);
                EXPECT_EQ(*round_trip, *decoded);
            }
        }
    }
}

TEST(SystemFileTest, EncoderRejectsChangedSectionBoundariesEvenWhenTotalSizeIsUnchanged) {
    for (const auto kind : {axk::SystemFileKind::a3000_system, axk::SystemFileKind::a4000_a5000_system2}) {
        const auto bytes = kind == axk::SystemFileKind::a3000_system ? a3000_system_record_fixture(0U, 0U)
                                                                     : system2_record_fixture(1U, 0U, 0U);
        const auto decoded = axk::decode_system_file(kind, bytes);
        ASSERT_TRUE(decoded);
        auto changed = *decoded;
        changed.system_header_bytes.push_back(changed.system_bulk_bytes.front());
        changed.system_bulk_bytes.erase(changed.system_bulk_bytes.begin());
        EXPECT_FALSE(axk::encode_system_file(changed));
        changed = *decoded;
        changed.reserved_tail_bytes.insert(changed.reserved_tail_bytes.begin(), changed.system_bulk_bytes.back());
        changed.system_bulk_bytes.pop_back();
        EXPECT_FALSE(axk::encode_system_file(changed));
        changed = *decoded;
        changed.system_header_bytes.clear();
        EXPECT_FALSE(axk::encode_system_file(changed));
        changed = *decoded;
        changed.system_bulk_bytes.clear();
        EXPECT_FALSE(axk::encode_system_file(changed));
    }
}

TEST(SystemFileTest, EncoderRejectsStaleDecodedMetadataRatherThanSilentlyIgnoringEdits) {
    const auto decoded =
        axk::decode_system_file(axk::SystemFileKind::a4000_a5000_system2, system2_record_fixture(1U, 1U, 31U));
    ASSERT_TRUE(decoded);
    auto changed = *decoded;
    changed.storage_revision = 0U;
    EXPECT_FALSE(axk::encode_system_file(changed));
    changed = *decoded;
    changed.record_envelope.type = axk::ObjectType::prog;
    EXPECT_FALSE(axk::encode_system_file(changed));
    changed = *decoded;
    changed.record_envelope.raw_type = "PROG";
    EXPECT_FALSE(axk::encode_system_file(changed));
    changed = *decoded;
    std::get<axk::A4000A5000SystemContext>(changed.context).saved_program_mode = axk::ProgramMode::single;
    EXPECT_FALSE(axk::encode_system_file(changed));
    changed = *decoded;
    changed.context = axk::A3000SystemContext{};
    EXPECT_FALSE(axk::encode_system_file(changed));
    changed = *decoded;
    changed.system_header_bytes[0] = std::byte{};
    EXPECT_FALSE(axk::encode_system_file(changed));
    changed = *decoded;
    changed.system_header_bytes[0x0eU] = std::byte{2};
    EXPECT_FALSE(axk::encode_system_file(changed));
    changed = *decoded;
    changed.record_envelope.raw_bytes[0x0cU] = std::byte{'?'};
    EXPECT_FALSE(axk::encode_system_file(changed));
    changed = *decoded;
    changed.kind = static_cast<axk::SystemFileKind>(255U);
    EXPECT_FALSE(axk::encode_system_file(changed));
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
    EXPECT_TRUE(axk::is_partition_support_root_entry("sfserrlog"));
    EXPECT_TRUE(axk::is_partition_support_root_entry("sfserram"));
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
