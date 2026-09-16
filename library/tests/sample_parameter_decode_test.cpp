#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/sample_parameters.hpp"
#include "axklib/system_file.hpp"
#include "axklib/writer.hpp"
#include "axklib/writer_internal.hpp"

namespace {

std::vector<std::byte> block(axk::SampleParameterGeneration generation) {
    std::vector<std::byte> bytes(generation == axk::SampleParameterGeneration::a3000 ? 0xbcU : 0xe0U);
    bytes[0x2e] = std::byte{60};
    bytes[0x2f] = std::byte{72};
    bytes[0x3a] = std::byte{128};
    bytes[0x3b] = std::byte{255};
    bytes[0x7a] = std::byte{26};
    bytes[0x7b] = std::byte{64};
    bytes[0x7c] = std::byte{10};
    axk::ByteWriter writer{bytes};
    EXPECT_TRUE(writer.write_be16(0x30, 44'100));
    EXPECT_TRUE(writer.write_be16(0x32, 22'050));
    EXPECT_TRUE(writer.write_be16(0x36, 0x1234));
    EXPECT_TRUE(writer.write_be16(0x38, 0xabcd));
    EXPECT_TRUE(writer.write_be16(0x3e, 9000));
    for (std::size_t index = 0; index < 8U; ++index)
        EXPECT_TRUE(writer.write_be32(0x40U + index * 4U, static_cast<std::uint32_t>(100U + index)));
    return bytes;
}

TEST(SampleParameterDecode, RequiresExactExplicitGenerationLayout) {
    for (const auto generation : {axk::SampleParameterGeneration::a3000, axk::SampleParameterGeneration::current}) {
        auto bytes = block(generation);
        EXPECT_TRUE(axk::decode_sample_parameter_block(bytes, generation));
        bytes.pop_back();
        EXPECT_FALSE(axk::decode_sample_parameter_block(bytes, generation));
        bytes.insert(bytes.end(), 2U, std::byte{});
        EXPECT_FALSE(axk::decode_sample_parameter_block(bytes, generation));
    }
    EXPECT_FALSE(axk::decode_sample_parameter_block(block(axk::SampleParameterGeneration::current),
                                                    static_cast<axk::SampleParameterGeneration>(99)));
}

TEST(SampleParameterDecode, KeepsBothLanesDerivedWordsAndAllRawBytes) {
    auto bytes = block(axk::SampleParameterGeneration::current);
    bytes[0x34] = std::byte{0xc1};
    bytes[0x35] = std::byte{63};
    bytes[0x28] = std::byte{0xef};
    bytes[0x29] = std::byte{0xbf};
    bytes[0xdd] = std::byte{0xf5};
    axk::ByteWriter writer{bytes};
    ASSERT_TRUE(writer.write_be32(0x18, 0x80000001));
    ASSERT_TRUE(writer.write_be32(0x24, 0x12345678));
    ASSERT_TRUE(writer.write_be32(0xb4, 0xffffffff));
    ASSERT_TRUE(writer.write_be32(0xb8, 0x98765432));
    constexpr std::array<std::uint16_t, 5> coefficients{0x8000, 0xffff, 0, 0x2000, 0x7fff};
    for (std::size_t index = 0; index < coefficients.size(); ++index)
        ASSERT_TRUE(writer.write_be16(0xaa + 2U * index, coefficients[index]));
    const auto before = bytes;

    const auto decoded = axk::decode_sample_parameter_block(bytes, axk::SampleParameterGeneration::current);

    ASSERT_TRUE(decoded);
    EXPECT_EQ(bytes, before);
    EXPECT_EQ(decoded->raw_bytes, before);
    EXPECT_EQ(decoded->sample_flags, 0xef);
    EXPECT_EQ(decoded->mapout_flags, 0xbf);
    EXPECT_EQ(decoded->parameters.sample_eq_type, 2);
    EXPECT_EQ(decoded->parameters.root_key, 60);
    EXPECT_EQ(decoded->parameters.fine_tune_cents, -63);
    EXPECT_EQ(decoded->parameters.key_low, 255);
    EXPECT_EQ(decoded->parameters.key_high, 128);
    EXPECT_EQ(decoded->parameters.loop_start_frame, 104U);
    EXPECT_EQ(decoded->parameters.loop_length_frames, 106U);
    EXPECT_EQ(decoded->members[0], (axk::SampleParameterMemberState{60, 44'100, -63, 0x1234, 100, 102, 104, 106}));
    EXPECT_EQ(decoded->members[1], (axk::SampleParameterMemberState{72, 22'050, 63, 0xabcd, 101, 103, 105, 107}));
    EXPECT_EQ(decoded->linked_program_bitmap_words[0], 0x80000001U);
    EXPECT_EQ(decoded->linked_program_bitmap_words[3], 0x12345678U);
    EXPECT_EQ(decoded->eq_coefficients, (std::array<std::int16_t, 5>{-32768, -1, 0, 8192, 32767}));
    EXPECT_EQ(decoded->cached_wave_end, 0xffffffffU);
    EXPECT_EQ(decoded->cached_loop_end, 0x98765432U);
}

TEST(SampleParameterDecode, UsesCanonicalControllersAndGenerationSpecificOutputs) {
    for (const auto generation : {axk::SampleParameterGeneration::a3000, axk::SampleParameterGeneration::current}) {
        auto bytes = block(generation);
        const auto current = generation == axk::SampleParameterGeneration::current;
        for (std::size_t index = 0; index < 6U; ++index) {
            bytes[index * 4U] = std::byte{125};
            bytes[index * 4U + 1U] = std::byte{21};
            bytes[index * 4U + 2U] = std::byte{3};
            bytes[index * 4U + 3U] = std::byte{0xc1};
            if (current) {
                bytes[0xbcU + index * 4U] = std::byte{126};
                bytes[0xbdU + index * 4U] = std::byte{36};
                bytes[0xbeU + index * 4U] = std::byte{2};
                bytes[0xbfU + index * 4U] = std::byte{63};
            }
        }
        bytes[0xa5] = std::byte{4};
        bytes[0xa6] = std::byte{71};
        bytes[0xa7] = std::byte{5};
        bytes[0xa8] = std::byte{72};
        if (current) {
            bytes[0xd6] = std::byte{12};
            bytes[0xd7] = std::byte{81};
            bytes[0xd8] = std::byte{11};
            bytes[0xd9] = std::byte{82};
        }
        const auto decoded = axk::decode_sample_parameter_block(bytes, generation);
        ASSERT_TRUE(decoded);
        for (const auto &control : decoded->parameters.controls) {
            EXPECT_EQ(control.device, current ? 126 : 125);
            EXPECT_EQ(control.function, current ? 36 : 21);
            EXPECT_EQ(control.type, current ? 2 : 3);
            EXPECT_EQ(control.range, current ? 63 : -63);
        }
        EXPECT_EQ(decoded->parameters.output1_destination, current ? 12 : 4);
        EXPECT_EQ(decoded->parameters.output1_level, current ? 81 : 71);
        EXPECT_EQ(decoded->parameters.output2_destination, current ? 11 : 5);
        EXPECT_EQ(decoded->parameters.output2_level, current ? 82 : 72);
        if (current) {
            EXPECT_EQ(decoded->controller_copies_match, false);
            std::copy_n(bytes.begin(), 0x18, bytes.begin() + 0xbc);
            EXPECT_EQ(axk::decode_sample_parameter_block(bytes, generation)->controller_copies_match, true);
        } else {
            EXPECT_FALSE(decoded->controller_copies_match);
            EXPECT_EQ(decoded->parameters.portamento_type, std::to_integer<std::uint8_t>(bytes[0x29]) & 1U);
            EXPECT_FALSE(decoded->parameters.portamento_rate);
            EXPECT_FALSE(decoded->parameters.portamento_time);
            EXPECT_FALSE(decoded->parameters.velocity_xfade_high);
            EXPECT_FALSE(decoded->parameters.velocity_xfade_low);
            EXPECT_FALSE(decoded->parameters.sample_eq_type);
        }
    }
}

TEST(SampleParameterDecode, RetainsInvalidLeavesWithoutAcceptingAnotherGenerationsBounds) {
    auto bytes = block(axk::SampleParameterGeneration::a3000);
    bytes[0x2b] = std::byte{13};
    bytes[0x2d] = std::byte{0x81};
    bytes[0x9b] = std::byte{2};
    bytes[0] = std::byte{126};
    bytes[1] = std::byte{22};
    bytes[0xa5] = std::byte{5};
    bytes[0xa7] = std::byte{6};
    auto decoded = axk::decode_sample_parameter_block(bytes, axk::SampleParameterGeneration::a3000);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->parameters.pitch_bend_type, 13);
    EXPECT_EQ(decoded->parameters.coarse_tune, -127);
    EXPECT_FALSE(decoded->parameters.aeg.attack_mode);
    EXPECT_FALSE(decoded->parameters.controls[0].device);
    EXPECT_FALSE(decoded->parameters.controls[0].function);
    EXPECT_FALSE(decoded->parameters.output1_destination);
    EXPECT_FALSE(decoded->parameters.output2_destination);
    EXPECT_EQ(decoded->raw_bytes, bytes);
    bytes.resize(0xe0);
    decoded = axk::decode_sample_parameter_block(bytes, axk::SampleParameterGeneration::current);
    ASSERT_TRUE(decoded);
    EXPECT_FALSE(decoded->parameters.pitch_bend_type);
    EXPECT_FALSE(decoded->parameters.coarse_tune);
    EXPECT_EQ(decoded->parameters.aeg.attack_mode, 2);
    for (const auto offset :
         {0x2a, 0x2e, 0x34, 0x3a, 0x3b, 0x3d, 0x62, 0x69, 0x6f, 0x7a, 0x7b, 0x7c, 0x85, 0x93, 0x9e, 0x9f, 0xda})
        bytes[static_cast<std::size_t>(offset)] = std::byte{0x80};
    decoded = axk::decode_sample_parameter_block(bytes, axk::SampleParameterGeneration::current);
    ASSERT_TRUE(decoded);
    const auto &p = decoded->parameters;
    EXPECT_FALSE(p.midi_receive_channel);
    EXPECT_FALSE(p.root_key);
    EXPECT_FALSE(p.fine_tune_cents);
    EXPECT_EQ(p.key_high, 128);
    EXPECT_FALSE(p.key_low);
    EXPECT_FALSE(p.loop_mode);
    EXPECT_FALSE(p.filter_cutoff);
    EXPECT_FALSE(p.filter_velocity_to_q_width);
    EXPECT_FALSE(p.pan);
    EXPECT_FALSE(p.sample_eq_frequency);
    EXPECT_FALSE(p.sample_eq_gain_db);
    EXPECT_FALSE(p.sample_eq_width_tenths);
    EXPECT_FALSE(p.feg.rate_key_scaling);
    EXPECT_FALSE(p.peg.range);
    EXPECT_FALSE(p.lfo.wave);
    EXPECT_FALSE(p.lfo.speed);
    EXPECT_FALSE(p.portamento_type);
    EXPECT_EQ(decoded->raw_bytes, bytes);
}

TEST(SampleParameterDecode, RegisteredSampleUsesCorrectBulkOffsetsWithoutMutation) {
    for (const auto kind : {axk::SystemFileKind::a3000_system, axk::SystemFileKind::a4000_a5000_system2}) {
        for (std::uint8_t revision = 0; revision <= 1U; ++revision) {
            const auto native = kind == axk::SystemFileKind::a3000_system;
            if (native && revision != 0U)
                continue;
            axk::DecodedSystemFile file;
            file.kind = kind;
            file.storage_revision = revision;
            file.system_bulk_bytes.resize(native ? 0x348U : 0xfe0U, std::byte{0xe3});
            const auto bytes =
                block(native ? axk::SampleParameterGeneration::a3000 : axk::SampleParameterGeneration::current);
            std::ranges::copy(bytes, file.system_bulk_bytes.begin() + (native ? 0x1c8 : 0x3ec));
            const auto before = file;
            const auto decoded = axk::decode_system_registered_sample(file);
            ASSERT_TRUE(decoded);
            EXPECT_EQ(decoded->raw_bytes, bytes);
            EXPECT_EQ(file, before);
            file.storage_revision = 2;
            EXPECT_FALSE(axk::decode_system_registered_sample(file));
            file = before;
            file.system_bulk_bytes.pop_back();
            EXPECT_FALSE(axk::decode_system_registered_sample(file));
        }
    }
    axk::DecodedSystemFile unknown;
    unknown.kind = static_cast<axk::SystemFileKind>(99);
    EXPECT_FALSE(axk::decode_system_registered_sample(unknown));
}

TEST(SampleParameterDecode, ReadsEveryAuthoredCommonLeafAtItsStoredOffset) {
    const auto check = [](const axk::SampleParameters &edit, auto read, std::size_t offset, std::uint8_t raw) {
        auto bytes = block(axk::SampleParameterGeneration::current);
        // Keep retained partners valid while checking individual leaf encodings.
        for (const auto upper : {0x65U, 0x72U, 0x75U})
            bytes[upper] = std::byte{127};
        ASSERT_TRUE(axk::detail::apply_sample_parameters_to_block(bytes, edit));
        EXPECT_EQ(std::to_integer<std::uint8_t>(bytes[offset]), raw);
        const auto decoded = axk::decode_sample_parameter_block(bytes, axk::SampleParameterGeneration::current);
        ASSERT_TRUE(decoded);
        EXPECT_EQ(read(decoded->parameters), read(edit));
        EXPECT_EQ(decoded->raw_bytes, bytes);
    };
#define AXK_CHECK_LEAF(member, value, offset, raw)                                                                     \
    {                                                                                                                  \
        SCOPED_TRACE(#member);                                                                                         \
        axk::SampleParameters edit;                                                                                    \
        edit.member = value;                                                                                           \
        check(edit, [](const axk::SampleParameters &p) { return p.member; }, offset, raw);                             \
    }
    AXK_CHECK_LEAF(midi_receive_channel, 16, 0x2a, 16);
    AXK_CHECK_LEAF(pitch_bend_type, 12, 0x2b, 12);
    AXK_CHECK_LEAF(pitch_bend_range, 24, 0x2c, 24);
    AXK_CHECK_LEAF(coarse_tune, -64, 0x2d, 192);
    AXK_CHECK_LEAF(root_key, 61, 0x2e, 61);
    AXK_CHECK_LEAF(fine_tune_cents, -17, 0x34, 239);
    AXK_CHECK_LEAF(key_high, 128, 0x3a, 128);
    AXK_CHECK_LEAF(key_low, 255, 0x3b, 255);
    AXK_CHECK_LEAF(loop_mode, axk::AudioSamplerLoopMode::reverse_one_shot, 0x3d, 5);
    AXK_CHECK_LEAF(loop_tempo_hundredths, 12345, 0x3e, 0x30);
    AXK_CHECK_LEAF(loop_start_frame, 0x12345678U, 0x50, 0x12);
    AXK_CHECK_LEAF(loop_length_frames, 0x23456789U, 0x58, 0x23);
    AXK_CHECK_LEAF(wave_start_velocity_sensitivity, -63, 0x60, 193);
    AXK_CHECK_LEAF(filter_type, 16, 0x61, 16);
    AXK_CHECK_LEAF(filter_cutoff, 91, 0x62, 91);
    AXK_CHECK_LEAF(filter_q_width, 31, 0x63, 31);
    AXK_CHECK_LEAF(filter_scaling_break1, 27, 0x64, 27);
    AXK_CHECK_LEAF(filter_scaling_break2, 83, 0x65, 83);
    AXK_CHECK_LEAF(filter_scaling_cutoff1, -127, 0x66, 129);
    AXK_CHECK_LEAF(filter_scaling_cutoff2, 127, 0x67, 127);
    AXK_CHECK_LEAF(filter_velocity_to_cutoff, 68, 0x68, 68);
    AXK_CHECK_LEAF(filter_velocity_to_q_width, -63, 0x69, 193);
    AXK_CHECK_LEAF(expand_detune, -7, 0x6a, 249);
    AXK_CHECK_LEAF(expand_dephase, -61, 0x6b, 195);
    AXK_CHECK_LEAF(expand_width, 62, 0x6c, 62);
    AXK_CHECK_LEAF(random_pitch, 63, 0x6d, 63);
    AXK_CHECK_LEAF(level, 97, 0x6e, 97);
    AXK_CHECK_LEAF(pan, -64, 0x6f, 192);
    AXK_CHECK_LEAF(velocity_low_limit, 29, 0x70, 29);
    AXK_CHECK_LEAF(velocity_offset, -127, 0x71, 129);
    AXK_CHECK_LEAF(velocity_high, 115, 0x72, 115);
    AXK_CHECK_LEAF(velocity_low, 31, 0x73, 31);
    AXK_CHECK_LEAF(level_scaling_break1, 36, 0x74, 36);
    AXK_CHECK_LEAF(level_scaling_break2, 84, 0x75, 84);
    AXK_CHECK_LEAF(level_scaling_level1, 59, 0x76, 59);
    AXK_CHECK_LEAF(level_scaling_level2, 101, 0x77, 101);
    AXK_CHECK_LEAF(velocity_sensitivity, -127, 0x78, 129);
    AXK_CHECK_LEAF(alternate_group, 16, 0x79, 16);
    AXK_CHECK_LEAF(sample_eq_frequency, 58, 0x7a, 58);
    AXK_CHECK_LEAF(sample_eq_gain_db, -12, 0x7b, 52);
    AXK_CHECK_LEAF(sample_eq_width_tenths, 120, 0x7c, 120);
    AXK_CHECK_LEAF(filter_cutoff_distance, -63, 0x7d, 193);
    AXK_CHECK_LEAF(feg.attack_rate, 91, 0x7e, 91);
    AXK_CHECK_LEAF(feg.decay_rate, 92, 0x7f, 92);
    AXK_CHECK_LEAF(feg.release_rate, 93, 0x80, 93);
    AXK_CHECK_LEAF(feg.init_level, -127, 0x81, 129);
    AXK_CHECK_LEAF(feg.attack_level, 126, 0x82, 126);
    AXK_CHECK_LEAF(feg.sustain_level, -125, 0x83, 131);
    AXK_CHECK_LEAF(feg.release_level, 124, 0x84, 124);
    AXK_CHECK_LEAF(feg.rate_key_scaling, -7, 0x85, 249);
    AXK_CHECK_LEAF(feg.rate_velocity_sensitivity, -63, 0x86, 193);
    AXK_CHECK_LEAF(feg.attack_level_velocity_sensitivity, 62, 0x87, 62);
    AXK_CHECK_LEAF(feg.level_velocity_sensitivity, -61, 0x88, 195);
    AXK_CHECK_LEAF(peg.attack_rate, 101, 0x89, 101);
    AXK_CHECK_LEAF(peg.decay_rate, 102, 0x8a, 102);
    AXK_CHECK_LEAF(peg.release_rate, 103, 0x8b, 103);
    AXK_CHECK_LEAF(peg.init_level, -117, 0x8c, 139);
    AXK_CHECK_LEAF(peg.attack_level, 116, 0x8d, 116);
    AXK_CHECK_LEAF(peg.sustain_level, -115, 0x8e, 141);
    AXK_CHECK_LEAF(peg.release_level, 114, 0x8f, 114);
    AXK_CHECK_LEAF(peg.rate_key_scaling, 7, 0x90, 7);
    AXK_CHECK_LEAF(peg.rate_velocity_sensitivity, 53, 0x91, 53);
    AXK_CHECK_LEAF(peg.level_velocity_sensitivity, -51, 0x92, 205);
    AXK_CHECK_LEAF(peg.range, -63, 0x93, 193);
    AXK_CHECK_LEAF(aeg.attack_rate, 111, 0x94, 111);
    AXK_CHECK_LEAF(aeg.decay_rate, 112, 0x95, 112);
    AXK_CHECK_LEAF(aeg.release_rate, 113, 0x96, 113);
    AXK_CHECK_LEAF(aeg.sustain_level, 114, 0x99, 114);
    AXK_CHECK_LEAF(aeg.attack_mode, 2, 0x9b, 2);
    AXK_CHECK_LEAF(aeg.rate_key_scaling, -6, 0x9c, 250);
    AXK_CHECK_LEAF(aeg.rate_velocity_sensitivity, -53, 0x9d, 203);
    AXK_CHECK_LEAF(lfo.wave, 3, 0x9e, 3);
    AXK_CHECK_LEAF(lfo.speed, 128, 0x9f, 127);
    AXK_CHECK_LEAF(lfo.speed, 1, 0x9f, 0);
    AXK_CHECK_LEAF(lfo.delay_time, 115, 0xa0, 115);
    AXK_CHECK_LEAF(lfo.cutoff_mod_depth, 116, 0xa2, 116);
    AXK_CHECK_LEAF(lfo.pitch_mod_depth, 117, 0xa3, 117);
    AXK_CHECK_LEAF(lfo.amp_mod_depth, 118, 0xa4, 118);
    AXK_CHECK_LEAF(filter_gain, -31, 0xa9, 225);
    AXK_CHECK_LEAF(velocity_xfade_high, 71, 0xd4, 71);
    AXK_CHECK_LEAF(velocity_xfade_low, 72, 0xd5, 72);
    AXK_CHECK_LEAF(output1_destination, 12, 0xd6, 12);
    AXK_CHECK_LEAF(output1_level, 73, 0xd7, 73);
    AXK_CHECK_LEAF(output2_destination, 11, 0xd8, 11);
    AXK_CHECK_LEAF(output2_level, 74, 0xd9, 74);
    AXK_CHECK_LEAF(portamento_type, 5, 0xda, 5);
    AXK_CHECK_LEAF(portamento_rate, 1, 0xdb, 1);
    AXK_CHECK_LEAF(portamento_time, 127, 0xdc, 127);
#undef AXK_CHECK_LEAF
}

TEST(SampleParameterDecode, PackedFlagsAreIndependentOfUnownedBits) {
    for (const auto generation : {axk::SampleParameterGeneration::a3000, axk::SampleParameterGeneration::current}) {
        for (unsigned raw = 0; raw <= 255U; ++raw) {
            auto bytes = block(generation);
            bytes[0x29] = static_cast<std::byte>(raw);
            bytes[0xa1] = static_cast<std::byte>(raw);
            const auto decoded = axk::decode_sample_parameter_block(bytes, generation);
            ASSERT_TRUE(decoded);
            const auto &p = decoded->parameters;
            EXPECT_EQ(p.fixed_pitch, (raw & 0x10U) != 0U);
            EXPECT_EQ(p.key_crossfade, (raw & 4U) != 0U);
            EXPECT_EQ(p.mono_mode, (raw & 2U) != 0U);
            if (generation == axk::SampleParameterGeneration::a3000) {
                EXPECT_EQ(p.portamento_type, raw & 1U);
                EXPECT_EQ(p.velocity_crossfade, (raw & 8U) != 0U);
            } else {
                EXPECT_FALSE(p.velocity_crossfade);
                EXPECT_EQ(p.portamento_type, std::to_integer<std::uint8_t>(bytes[0xda]));
            }
            EXPECT_EQ(p.lfo.key_on_sync, (raw & 1U) != 0U);
            EXPECT_EQ(p.lfo.cutoff_mod_phase_invert, (raw & 2U) != 0U);
            EXPECT_EQ(p.lfo.pitch_mod_phase_invert, (raw & 4U) != 0U);
            if (generation == axk::SampleParameterGeneration::a3000 || (raw >> 6U) == 3U)
                EXPECT_FALSE(p.sample_eq_type);
            else
                EXPECT_EQ(p.sample_eq_type, raw >> 6U);
            EXPECT_EQ(decoded->raw_bytes, bytes);
        }
    }
}

} // namespace
