#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "axklib/bytes.hpp"
#include "axklib/object.hpp"
#include "axklib/sequence.hpp"

namespace {

void append_be16(std::vector<std::byte> &bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>(value >> 8U));
    bytes.push_back(static_cast<std::byte>(value));
}

void append_be32(std::vector<std::byte> &bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::byte>(value >> 24U));
    bytes.push_back(static_cast<std::byte>(value >> 16U));
    bytes.push_back(static_cast<std::byte>(value >> 8U));
    bytes.push_back(static_cast<std::byte>(value));
}

std::vector<std::byte> smf0(std::vector<std::byte> track, std::uint16_t division = 96U) {
    std::vector<std::byte> result{
        std::byte{'M'},
        std::byte{'T'},
        std::byte{'h'},
        std::byte{'d'},
    };
    append_be32(result, 6U);
    append_be16(result, 0U);
    append_be16(result, 1U);
    append_be16(result, division);
    result.insert(result.end(), {std::byte{'M'}, std::byte{'T'}, std::byte{'r'}, std::byte{'k'}});
    append_be32(result, static_cast<std::uint32_t>(track.size()));
    result.insert(result.end(), track.begin(), track.end());
    return result;
}

std::vector<std::byte> test_smf(std::uint16_t division = 96U) {
    return smf0(
        {
            std::byte{},     std::byte{0xff}, std::byte{0x03}, std::byte{4},    std::byte{'T'},  std::byte{'e'},
            std::byte{'s'},  std::byte{'t'},  std::byte{},     std::byte{0xff}, std::byte{0x51}, std::byte{3},
            std::byte{0x07}, std::byte{0xa1}, std::byte{0x20}, std::byte{},     std::byte{0x90}, std::byte{60},
            std::byte{100},  std::byte{96},   std::byte{60},   std::byte{},     std::byte{},     std::byte{0xff},
            std::byte{0x2f}, std::byte{},
        },
        division);
}

std::vector<std::byte> channel_family_smf() {
    return smf0({
        std::byte{0x83}, std::byte{0x40}, std::byte{0x90}, std::byte{61},   std::byte{127},  std::byte{},
        std::byte{64},   std::byte{96},   std::byte{1},    std::byte{0x80}, std::byte{61},   std::byte{},
        std::byte{},     std::byte{0xa0}, std::byte{61},   std::byte{1},    std::byte{},     std::byte{0xb0},
        std::byte{7},    std::byte{100},  std::byte{},     std::byte{0xc0}, std::byte{10},   std::byte{},
        std::byte{0xd0}, std::byte{20},   std::byte{},     std::byte{0xe0}, std::byte{},     std::byte{64},
        std::byte{},     std::byte{0xff}, std::byte{0x51}, std::byte{3},    std::byte{0x07}, std::byte{0xa1},
        std::byte{0x20}, std::byte{},     std::byte{0xff}, std::byte{0x2f}, std::byte{},
    });
}

} // namespace

TEST(CurrentSequence, DecodesTimelineAndExportsSmf0) {
    const auto payload = axk::smf0_to_current_sequence(test_smf(), "Sequence");
    ASSERT_TRUE(payload) << payload.error().message;
    const auto decoded = axk::decode_object(*payload);
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto &sequence = std::get<axk::CurrentSequence>(decoded->payload);
    EXPECT_EQ(sequence.format_version, 1U);
    EXPECT_EQ(sequence.ticks_per_quarter_note, 96U);
    EXPECT_EQ(sequence.first_tick, 0U);
    EXPECT_EQ(sequence.end_tick, 96U);
    EXPECT_EQ(sequence.event_count, 5U);
    EXPECT_EQ(sequence.tempo_bpm, 120U);
    ASSERT_EQ(sequence.events.size(), 5U);
    EXPECT_EQ(sequence.events[2].message, (std::vector<std::byte>{std::byte{0x90}, std::byte{60}, std::byte{100}}));
    EXPECT_EQ(sequence.events[3].message, (std::vector<std::byte>{std::byte{0x90}, std::byte{60}, std::byte{0}}));

    const auto exported = axk::sequence_to_smf0(sequence);
    ASSERT_TRUE(exported) << exported.error().message;
    const auto roundtrip = axk::smf0_to_current_sequence(*exported, "Roundtrip");
    ASSERT_TRUE(roundtrip) << roundtrip.error().message;
    const auto roundtrip_decoded = axk::decode_object(*roundtrip);
    ASSERT_TRUE(roundtrip_decoded);
    EXPECT_EQ(std::get<axk::CurrentSequence>(roundtrip_decoded->payload).events, sequence.events);
}

TEST(CurrentSequence, PreservesAdmittedChannelFamiliesAndNativeRunningStatus) {
    const auto payload = axk::smf0_to_current_sequence(channel_family_smf(), "All Channels");
    ASSERT_TRUE(payload) << payload.error().message;

    const axk::ByteReader reader{*payload};
    ASSERT_EQ(reader.be32(0x80U), 448U);
    ASSERT_EQ(reader.be32(0x84U), 449U);
    ASSERT_EQ(reader.be16(0x88U), 2U);
    EXPECT_EQ(payload->at(0x8aU), std::byte{0x90});
    EXPECT_EQ(payload->at(0x8eU), std::byte{64});

    const auto decoded = axk::decode_object(*payload);
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto &sequence = std::get<axk::CurrentSequence>(decoded->payload);
    ASSERT_EQ(sequence.events.size(), 10U);
    EXPECT_EQ(sequence.first_tick, 448U);
    EXPECT_EQ(sequence.end_tick, 449U);
    EXPECT_EQ(sequence.events[1].message, (std::vector<std::byte>{std::byte{0x90}, std::byte{64}, std::byte{96}}));
    EXPECT_EQ(sequence.events[3].message.front(), std::byte{0xa0});
    EXPECT_EQ(sequence.events[4].message.front(), std::byte{0xb0});
    EXPECT_EQ(sequence.events[5].message.front(), std::byte{0xc0});
    EXPECT_EQ(sequence.events[6].message.front(), std::byte{0xd0});
    EXPECT_EQ(sequence.events[7].message, (std::vector<std::byte>{std::byte{0xe0}, std::byte{64}}));

    const auto exported = axk::sequence_to_smf0(sequence);
    ASSERT_TRUE(exported) << exported.error().message;
    const auto roundtrip = axk::smf0_to_current_sequence(*exported, "Roundtrip");
    ASSERT_TRUE(roundtrip) << roundtrip.error().message;
    const auto roundtrip_decoded = axk::decode_object(*roundtrip);
    ASSERT_TRUE(roundtrip_decoded);
    EXPECT_EQ(std::get<axk::CurrentSequence>(roundtrip_decoded->payload).events, sequence.events);
}

TEST(CurrentSequence, NormalizesPpqnAndRejectsLossyMidiProfiles) {
    const auto normalized = axk::smf0_to_current_sequence(test_smf(480U), "Normalized");
    ASSERT_TRUE(normalized) << normalized.error().message;
    const auto decoded = axk::decode_object(*normalized);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(std::get<axk::CurrentSequence>(decoded->payload).end_tick, 19U);

    const auto sys_exclusive = smf0({std::byte{}, std::byte{0xf0}, std::byte{1}, std::byte{0x7e}, std::byte{},
                                     std::byte{0xff}, std::byte{0x2f}, std::byte{}});
    EXPECT_FALSE(axk::smf0_to_current_sequence(sys_exclusive, "SysEx"));

    const auto precise_pitch_bend = smf0({std::byte{}, std::byte{0xe0}, std::byte{1}, std::byte{64}, std::byte{},
                                          std::byte{0xff}, std::byte{0x2f}, std::byte{}});
    EXPECT_FALSE(axk::smf0_to_current_sequence(precise_pitch_bend, "Pitch"));

    const auto native_terminator_in_meta =
        smf0({std::byte{}, std::byte{0xff}, std::byte{1}, std::byte{1}, std::byte{0xfd}, std::byte{}, std::byte{0xff},
              std::byte{0x2f}, std::byte{}});
    EXPECT_FALSE(axk::smf0_to_current_sequence(native_terminator_in_meta, "Meta"));

    auto format_one = test_smf();
    format_one[9] = std::byte{1};
    EXPECT_FALSE(axk::smf0_to_current_sequence(format_one, "Format 1"));

    auto smpte = test_smf();
    smpte[12] = std::byte{0xe7};
    EXPECT_FALSE(axk::smf0_to_current_sequence(smpte, "SMPTE"));
    EXPECT_FALSE(axk::smf0_to_current_sequence(test_smf(), "Name\x7f"));
}

TEST(CurrentSequence, RejectsMalformedNativeTimeline) {
    const auto generated = axk::smf0_to_current_sequence(channel_family_smf(), "Sequence");
    ASSERT_TRUE(generated);

    auto missing_status = *generated;
    missing_status[0x8aU] = std::byte{61};
    EXPECT_FALSE(axk::decode_object(missing_status));

    auto empty_block = *generated;
    empty_block[0x88U] = std::byte{};
    empty_block[0x89U] = std::byte{};
    EXPECT_FALSE(axk::decode_object(empty_block));

    auto backwards_tick = *generated;
    backwards_tick[0x84U] = std::byte{};
    backwards_tick[0x85U] = std::byte{};
    backwards_tick[0x86U] = std::byte{1};
    backwards_tick[0x87U] = std::byte{0xbf};
    EXPECT_FALSE(axk::decode_object(backwards_tick));

    auto missing_terminator = *generated;
    missing_terminator[0x8dU] = std::byte{};
    EXPECT_FALSE(axk::decode_object(missing_terminator));

    auto truncated = *generated;
    truncated.resize(0x89U);
    EXPECT_FALSE(axk::decode_object(truncated));
}
