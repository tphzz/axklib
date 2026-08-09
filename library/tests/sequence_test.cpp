#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <span>
#include <string_view>
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

axk::Result<std::vector<std::byte>>
import_smf(std::span<const std::byte> smf, std::string_view name,
           axk::SequenceSystemExclusivePolicy policy = axk::SequenceSystemExclusivePolicy::reject) {
    return axk::smf0_to_current_sequence(smf, name, policy);
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

std::vector<std::byte> system_exclusive_smf() {
    return smf0({
        std::byte{},     std::byte{0x90}, std::byte{60},   std::byte{100},  std::byte{48},   std::byte{0xf0},
        std::byte{3},    std::byte{0x7d}, std::byte{0x01}, std::byte{0xf7}, std::byte{48},   std::byte{0xb0},
        std::byte{71},   std::byte{12},   std::byte{},     std::byte{0xb0}, std::byte{74},   std::byte{96},
        std::byte{0x60}, std::byte{0xf7}, std::byte{2},    std::byte{0x02}, std::byte{0x03}, std::byte{},
        std::byte{0x80}, std::byte{60},   std::byte{64},   std::byte{},     std::byte{0xff}, std::byte{0x2f},
        std::byte{},
    });
}

std::vector<std::byte> tempo_message(std::uint32_t microseconds_per_quarter_note) {
    return {std::byte{0xff}, std::byte{0x51}, static_cast<std::byte>(microseconds_per_quarter_note >> 16U),
            static_cast<std::byte>(microseconds_per_quarter_note >> 8U),
            static_cast<std::byte>(microseconds_per_quarter_note)};
}

std::vector<axk::SequenceTempoEvent> tempo_events(const axk::CurrentSequence &sequence) {
    std::vector<axk::SequenceTempoEvent> result;
    for (const auto &event : sequence.events) {
        if (event.kind != axk::SequenceEventKind::meta || event.message.size() != 5U ||
            event.message[0] != std::byte{0xff} || event.message[1] != std::byte{0x51}) {
            continue;
        }
        result.push_back({.tick = event.tick,
                          .microseconds_per_quarter_note = (std::to_integer<std::uint32_t>(event.message[2]) << 16U) |
                                                           (std::to_integer<std::uint32_t>(event.message[3]) << 8U) |
                                                           std::to_integer<std::uint32_t>(event.message[4])});
    }
    return result;
}

} // namespace

TEST(CurrentSequence, DecodesTimelineAndExportsSmf0) {
    const auto payload = import_smf(test_smf(), "Sequence", axk::SequenceSystemExclusivePolicy::reject);
    ASSERT_TRUE(payload) << payload.error().message;
    const auto decoded = axk::decode_object(*payload);
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto &sequence = std::get<axk::CurrentSequence>(decoded->payload);
    EXPECT_EQ(sequence.format_version, 1U);
    EXPECT_EQ(sequence.ticks_per_quarter_note, 96U);
    EXPECT_EQ(sequence.first_tick, 0U);
    EXPECT_EQ(sequence.end_tick, 96U);
    EXPECT_EQ(sequence.event_count, 5U);
    EXPECT_EQ(sequence.header_tempo_bpm, 120U);
    EXPECT_EQ(sequence.effective_initial_tempo_microseconds_per_quarter_note, 500'000U);
    EXPECT_EQ(sequence.tempo_events,
              (std::vector<axk::SequenceTempoEvent>{{.tick = 0U, .microseconds_per_quarter_note = 500'000U}}));
    ASSERT_EQ(sequence.events.size(), 5U);
    EXPECT_EQ(sequence.events[2].message, (std::vector<std::byte>{std::byte{0x90}, std::byte{60}, std::byte{100}}));
    EXPECT_EQ(sequence.events[3].message, (std::vector<std::byte>{std::byte{0x90}, std::byte{60}, std::byte{0}}));

    const auto exported = axk::sequence_to_smf0(sequence);
    ASSERT_TRUE(exported) << exported.error().message;
    const auto roundtrip = import_smf(*exported, "Roundtrip", axk::SequenceSystemExclusivePolicy::reject);
    ASSERT_TRUE(roundtrip) << roundtrip.error().message;
    const auto roundtrip_decoded = axk::decode_object(*roundtrip);
    ASSERT_TRUE(roundtrip_decoded);
    EXPECT_EQ(std::get<axk::CurrentSequence>(roundtrip_decoded->payload).events, sequence.events);
}

TEST(CurrentSequence, IgnoresTheStoredNextTickAfterEndOfTrack) {
    auto payload = import_smf(smf0({std::byte{}, std::byte{0xff}, std::byte{0x2f}, std::byte{}}), "Sequence",
                              axk::SequenceSystemExclusivePolicy::reject);
    ASSERT_TRUE(payload) << payload.error().message;
    ASSERT_GE(payload->size(), 0x88U);
    (*payload)[0x84U] = std::byte{0x12};
    (*payload)[0x85U] = std::byte{0x34};
    (*payload)[0x86U] = std::byte{0x56};
    (*payload)[0x87U] = std::byte{0x78};

    const auto decoded = axk::decode_object(*payload);
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto &sequence = std::get<axk::CurrentSequence>(decoded->payload);
    ASSERT_EQ(sequence.events.size(), 1U);
    EXPECT_EQ(sequence.events.front().kind, axk::SequenceEventKind::meta);
    EXPECT_EQ(sequence.end_tick, 0U);
}

TEST(CurrentSequence, InspectsControllersAndOpaqueSystemExclusiveWithoutReturningPayloads) {
    const auto inspected = axk::inspect_smf0(system_exclusive_smf());
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_EQ(inspected->format, 0U);
    EXPECT_EQ(inspected->track_count, 1U);
    EXPECT_EQ(inspected->ticks_per_quarter_note, 96U);
    EXPECT_EQ(inspected->end_tick, 192U);
    EXPECT_EQ(inspected->event_count, 7U);
    EXPECT_EQ(inspected->channel_event_count, 4U);
    EXPECT_EQ(inspected->meta_event_count, 1U);
    EXPECT_EQ(inspected->system_exclusive_event_count, 2U);
    EXPECT_EQ(inspected->system_exclusive_data_bytes, 5U);
    EXPECT_EQ(inspected->controllers,
              (std::vector<axk::SequenceMidiControllerCount>{{.controller = 71U, .event_count = 1U},
                                                             {.controller = 74U, .event_count = 1U}}));
    EXPECT_EQ(inspected->system_exclusive_manufacturer_ids, (std::vector<std::string>{"7D"}));
    EXPECT_TRUE(inspected->system_exclusive_preservation_supported);
}

TEST(CurrentSequence, RequiresAnExplicitSystemExclusiveImportPolicy) {
    const auto rejected =
        import_smf(system_exclusive_smf(), "Reject SysEx", axk::SequenceSystemExclusivePolicy::reject);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, axk::ErrorCode::unsupported_profile);
    EXPECT_NE(rejected.error().message.find("contains System Exclusive"), std::string::npos);

    const auto preserved =
        import_smf(system_exclusive_smf(), "Preserve SysEx", axk::SequenceSystemExclusivePolicy::preserve);
    ASSERT_TRUE(preserved) << preserved.error().message;
    const auto decoded = axk::decode_object(*preserved);
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto &sequence = std::get<axk::CurrentSequence>(decoded->payload);
    ASSERT_EQ(sequence.events.size(), 7U);
    EXPECT_EQ(sequence.events[1],
              (axk::SequenceEvent{.tick = 48U,
                                  .kind = axk::SequenceEventKind::system_exclusive,
                                  .message = {std::byte{0xf0}, std::byte{0x7d}, std::byte{0x01}, std::byte{0xf7}}}));
    EXPECT_EQ(sequence.events[4], (axk::SequenceEvent{.tick = 192U,
                                                      .kind = axk::SequenceEventKind::system_exclusive,
                                                      .message = {std::byte{0xf7}, std::byte{0x02}, std::byte{0x03}}}));

    const auto exported = axk::sequence_to_smf0(sequence);
    ASSERT_TRUE(exported) << exported.error().message;
    const auto reimported = import_smf(*exported, "SysEx Roundtrip", axk::SequenceSystemExclusivePolicy::preserve);
    ASSERT_TRUE(reimported) << reimported.error().message;
    const auto redecoded = axk::decode_object(*reimported);
    ASSERT_TRUE(redecoded) << redecoded.error().message;
    std::vector<axk::SequenceEvent> redecoded_system_exclusive;
    std::ranges::copy_if(
        std::get<axk::CurrentSequence>(redecoded->payload).events, std::back_inserter(redecoded_system_exclusive),
        [](const axk::SequenceEvent &event) { return event.kind == axk::SequenceEventKind::system_exclusive; });
    EXPECT_EQ(redecoded_system_exclusive, (std::vector<axk::SequenceEvent>{sequence.events[1], sequence.events[4]}));
}

TEST(CurrentSequence, RejectsSystemExclusiveOutsideTheNativeDelimiterProfile) {
    const auto unterminated = smf0({std::byte{}, std::byte{0xf0}, std::byte{2}, std::byte{0x7d}, std::byte{0x01},
                                    std::byte{}, std::byte{0xff}, std::byte{0x2f}, std::byte{}});
    const auto inspected = axk::inspect_smf0(unterminated);
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_FALSE(inspected->system_exclusive_preservation_supported);
    const auto rejected = import_smf(unterminated, "Unterminated", axk::SequenceSystemExclusivePolicy::preserve);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, axk::ErrorCode::unsupported_profile);
}

TEST(CurrentSequence, ExcludesSystemExclusiveWithoutChangingRetainedEventTicks) {
    const auto payload =
        import_smf(system_exclusive_smf(), "Exclude SysEx", axk::SequenceSystemExclusivePolicy::exclude);
    ASSERT_TRUE(payload) << payload.error().message;
    const auto decoded = axk::decode_object(*payload);
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto &sequence = std::get<axk::CurrentSequence>(decoded->payload);
    ASSERT_EQ(sequence.events.size(), 5U);
    EXPECT_EQ(sequence.end_tick, 192U);
    EXPECT_EQ(sequence.events[0].tick, 0U);
    EXPECT_EQ(sequence.events[1].tick, 96U);
    EXPECT_EQ(sequence.events[1].message, (std::vector<std::byte>{std::byte{0xb0}, std::byte{71}, std::byte{12}}));
    EXPECT_EQ(sequence.events[2].tick, 96U);
    EXPECT_EQ(sequence.events[2].message, (std::vector<std::byte>{std::byte{0xb0}, std::byte{74}, std::byte{96}}));
    EXPECT_EQ(sequence.events[3].tick, 192U);
    EXPECT_EQ(sequence.events[4].tick, 192U);
    EXPECT_EQ(sequence.events[4].message, (std::vector<std::byte>{std::byte{0xff}, std::byte{0x2f}}));
    EXPECT_TRUE(std::ranges::none_of(
        sequence.events, [](const auto &event) { return event.kind == axk::SequenceEventKind::system_exclusive; }));
}

TEST(CurrentSequence, InjectsHeaderTempoOnlyWhenTimelineHasNoTickZeroTempo) {
    const auto no_tempo =
        smf0({std::byte{}, std::byte{0x90}, std::byte{60}, std::byte{100}, std::byte{96}, std::byte{0x80},
              std::byte{60}, std::byte{}, std::byte{}, std::byte{0xff}, std::byte{0x2f}, std::byte{}});
    auto payload = import_smf(no_tempo, "Header only");
    ASSERT_TRUE(payload) << payload.error().message;
    axk::ByteWriter writer{*payload};
    ASSERT_TRUE(writer.write_be16(0x6cU, 137U));
    ASSERT_TRUE(writer.write_be16(0x78U, 137U));

    const auto decoded = axk::decode_object(*payload);
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto &sequence = std::get<axk::CurrentSequence>(decoded->payload);
    EXPECT_EQ(sequence.header_tempo_bpm, 137U);
    EXPECT_EQ(sequence.effective_initial_tempo_microseconds_per_quarter_note, 437'956U);
    EXPECT_TRUE(sequence.tempo_events.empty());

    const auto exported = axk::sequence_to_smf0(sequence);
    ASSERT_TRUE(exported) << exported.error().message;
    const auto reimported = import_smf(*exported, "Reimported");
    ASSERT_TRUE(reimported) << reimported.error().message;
    const auto roundtrip = axk::decode_object(*reimported);
    ASSERT_TRUE(roundtrip) << roundtrip.error().message;
    const auto &roundtrip_sequence = std::get<axk::CurrentSequence>(roundtrip->payload);
    EXPECT_EQ(roundtrip_sequence.header_tempo_bpm, 137U);
    EXPECT_EQ(tempo_events(roundtrip_sequence),
              (std::vector<axk::SequenceTempoEvent>{{.tick = 0U, .microseconds_per_quarter_note = 437'956U}}));
}

TEST(CurrentSequence, PreservesPreciseTimelineTempoInsteadOfRoundingItToTheHeader) {
    const auto dj_tempo = smf0({
        std::byte{},     std::byte{0xff}, std::byte{0x51}, std::byte{3},    std::byte{0x07}, std::byte{0x05},
        std::byte{0x5a}, std::byte{},     std::byte{0xff}, std::byte{0x58}, std::byte{4},    std::byte{4},
        std::byte{2},    std::byte{24},   std::byte{8},    std::byte{},     std::byte{0x90}, std::byte{60},
        std::byte{100},  std::byte{96},   std::byte{0x80}, std::byte{60},   std::byte{},     std::byte{},
        std::byte{0xff}, std::byte{0x2f}, std::byte{},
    });
    const auto payload = import_smf(dj_tempo, "DJ tempo");
    ASSERT_TRUE(payload) << payload.error().message;
    const auto decoded = axk::decode_object(*payload);
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto &sequence = std::get<axk::CurrentSequence>(decoded->payload);
    EXPECT_EQ(sequence.header_tempo_bpm, 130U);
    EXPECT_EQ(sequence.effective_initial_tempo_microseconds_per_quarter_note, 460'122U);

    const auto exported = axk::sequence_to_smf0(sequence);
    ASSERT_TRUE(exported) << exported.error().message;
    const auto reimported = import_smf(*exported, "Reimported");
    ASSERT_TRUE(reimported) << reimported.error().message;
    const auto roundtrip = axk::decode_object(*reimported);
    ASSERT_TRUE(roundtrip) << roundtrip.error().message;
    const auto &roundtrip_sequence = std::get<axk::CurrentSequence>(roundtrip->payload);
    EXPECT_EQ(tempo_events(roundtrip_sequence),
              (std::vector<axk::SequenceTempoEvent>{{.tick = 0U, .microseconds_per_quarter_note = 460'122U}}));
    EXPECT_EQ(roundtrip_sequence.events, sequence.events);
}

TEST(CurrentSequence, PreservesTempoMapTimeSignaturesAndSameTickTempoPrecedence) {
    const auto mapped =
        smf0({std::byte{},     std::byte{0xff}, std::byte{0x51}, std::byte{3},    std::byte{0x07}, std::byte{0xa1},
              std::byte{0x20}, std::byte{},     std::byte{0xff}, std::byte{0x51}, std::byte{3},    std::byte{0x07},
              std::byte{0x05}, std::byte{0x5a}, std::byte{},     std::byte{0xff}, std::byte{0x58}, std::byte{4},
              std::byte{3},    std::byte{2},    std::byte{24},   std::byte{8},    std::byte{0x83}, std::byte{0x60},
              std::byte{0xff}, std::byte{0x51}, std::byte{3},    std::byte{0x06}, std::byte{0x1a}, std::byte{0x80},
              std::byte{},     std::byte{0xff}, std::byte{0x2f}, std::byte{}},
             480U);
    const auto payload = import_smf(mapped, "Tempo map");
    ASSERT_TRUE(payload) << payload.error().message;
    const auto decoded = axk::decode_object(*payload);
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto &sequence = std::get<axk::CurrentSequence>(decoded->payload);
    EXPECT_EQ(sequence.header_tempo_bpm, 130U);
    EXPECT_EQ(sequence.effective_initial_tempo_microseconds_per_quarter_note, 460'122U);
    EXPECT_EQ(sequence.tempo_events,
              (std::vector<axk::SequenceTempoEvent>{{.tick = 0U, .microseconds_per_quarter_note = 500'000U},
                                                    {.tick = 0U, .microseconds_per_quarter_note = 460'122U},
                                                    {.tick = 96U, .microseconds_per_quarter_note = 400'000U}}));

    const auto exported = axk::sequence_to_smf0(sequence);
    ASSERT_TRUE(exported) << exported.error().message;
    const auto reimported = import_smf(*exported, "Reimported");
    ASSERT_TRUE(reimported) << reimported.error().message;
    const auto roundtrip = axk::decode_object(*reimported);
    ASSERT_TRUE(roundtrip) << roundtrip.error().message;
    EXPECT_EQ(std::get<axk::CurrentSequence>(roundtrip->payload).events, sequence.events);
}

TEST(CurrentSequence, LaterTempoChangeDoesNotReplaceTheInitialHeaderTempo) {
    const auto later_tempo = smf0({std::byte{}, std::byte{0x90}, std::byte{60}, std::byte{100}, std::byte{96},
                                   std::byte{0xff}, std::byte{0x51}, std::byte{3}, std::byte{0x06}, std::byte{0x1a},
                                   std::byte{0x80}, std::byte{}, std::byte{0xff}, std::byte{0x2f}, std::byte{}});
    const auto payload = import_smf(later_tempo, "Later tempo");
    ASSERT_TRUE(payload) << payload.error().message;
    const auto decoded = axk::decode_object(*payload);
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto &sequence = std::get<axk::CurrentSequence>(decoded->payload);
    EXPECT_EQ(sequence.header_tempo_bpm, 120U);
    EXPECT_EQ(sequence.effective_initial_tempo_microseconds_per_quarter_note, 500'000U);
    EXPECT_EQ(sequence.tempo_events,
              (std::vector<axk::SequenceTempoEvent>{{.tick = 96U, .microseconds_per_quarter_note = 400'000U}}));

    const auto exported = axk::sequence_to_smf0(sequence);
    ASSERT_TRUE(exported) << exported.error().message;
    const auto reimported = import_smf(*exported, "Reimported");
    ASSERT_TRUE(reimported) << reimported.error().message;
    const auto roundtrip = axk::decode_object(*reimported);
    ASSERT_TRUE(roundtrip) << roundtrip.error().message;
    EXPECT_EQ(tempo_events(std::get<axk::CurrentSequence>(roundtrip->payload)),
              (std::vector<axk::SequenceTempoEvent>{{.tick = 0U, .microseconds_per_quarter_note = 500'000U},
                                                    {.tick = 96U, .microseconds_per_quarter_note = 400'000U}}));
}

TEST(CurrentSequence, RejectsMalformedZeroAndOutOfProfileTempoEvents) {
    const auto midi_with_tempo = [](std::uint8_t length, std::uint32_t micros) {
        auto track =
            std::vector<std::byte>{std::byte{}, std::byte{0xff}, std::byte{0x51}, static_cast<std::byte>(length)};
        for (std::uint8_t index = 0; index < length; ++index)
            track.push_back(static_cast<std::byte>(micros >> (8U * (length - index - 1U))));
        track.insert(track.end(), {std::byte{}, std::byte{0xff}, std::byte{0x2f}, std::byte{}});
        return smf0(std::move(track));
    };
    EXPECT_FALSE(import_smf(midi_with_tempo(2U, 500'000U), "Short"));
    EXPECT_FALSE(import_smf(midi_with_tempo(3U, 0U), "Zero"));
    EXPECT_FALSE(import_smf(midi_with_tempo(3U, 199'999U), "Too fast"));
    EXPECT_FALSE(import_smf(midi_with_tempo(3U, 2'000'001U), "Too slow"));
    EXPECT_TRUE(import_smf(midi_with_tempo(3U, 200'000U), "Fast edge"));
    EXPECT_TRUE(import_smf(midi_with_tempo(3U, 2'000'000U), "Slow edge"));

    const auto valid = import_smf(test_smf(), "Native");
    ASSERT_TRUE(valid);
    const auto tempo = std::ranges::search(*valid, tempo_message(500'000U));
    ASSERT_NE(tempo.begin(), valid->end());
    auto malformed_native = *valid;
    malformed_native[static_cast<std::size_t>(tempo.begin() - valid->begin()) + 4U] = std::byte{0xfd};
    EXPECT_FALSE(axk::decode_object(malformed_native));

    const auto decoded = axk::decode_object(*valid);
    ASSERT_TRUE(decoded);
    auto invalid_header = std::get<axk::CurrentSequence>(decoded->payload);
    invalid_header.events.erase(std::remove_if(invalid_header.events.begin(), invalid_header.events.end(),
                                               [](const axk::SequenceEvent &event) {
                                                   return event.kind == axk::SequenceEventKind::meta &&
                                                          event.message.size() >= 2U &&
                                                          event.message[0] == std::byte{0xff} &&
                                                          event.message[1] == std::byte{0x51};
                                               }),
                                invalid_header.events.end());
    invalid_header.header_tempo_bpm = 0U;
    EXPECT_FALSE(axk::sequence_to_smf0(invalid_header));
    invalid_header.header_tempo_bpm = 301U;
    EXPECT_FALSE(axk::sequence_to_smf0(invalid_header));
}

TEST(CurrentSequence, PreservesAdmittedChannelFamiliesAndNativeRunningStatus) {
    const auto payload = import_smf(channel_family_smf(), "All Channels");
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
    const auto roundtrip = import_smf(*exported, "Roundtrip");
    ASSERT_TRUE(roundtrip) << roundtrip.error().message;
    const auto roundtrip_decoded = axk::decode_object(*roundtrip);
    ASSERT_TRUE(roundtrip_decoded);
    auto expected_events = sequence.events;
    expected_events.insert(expected_events.begin(),
                           {.tick = 0U, .kind = axk::SequenceEventKind::meta, .message = tempo_message(500'000U)});
    EXPECT_EQ(std::get<axk::CurrentSequence>(roundtrip_decoded->payload).events, expected_events);
}

TEST(CurrentSequence, PreservesLongNotesWithinTheSmfDeltaLimitAndRejectsLargerGaps) {
    constexpr std::uint32_t long_note_ticks = 0x0020'0000U;
    const auto long_note = smf0({std::byte{}, std::byte{0x90}, std::byte{60}, std::byte{100}, std::byte{0x81},
                                 std::byte{0x80}, std::byte{0x80}, std::byte{}, std::byte{0x80}, std::byte{60},
                                 std::byte{}, std::byte{}, std::byte{0xff}, std::byte{0x2f}, std::byte{}});
    const auto payload = import_smf(long_note, "Long Note");
    ASSERT_TRUE(payload) << payload.error().message;
    const auto decoded = axk::decode_object(*payload);
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto &sequence = std::get<axk::CurrentSequence>(decoded->payload);
    ASSERT_EQ(sequence.events.size(), 3U);
    EXPECT_EQ(sequence.events[1].tick, long_note_ticks);
    const auto exported = axk::sequence_to_smf0(sequence);
    ASSERT_TRUE(exported) << exported.error().message;

    auto unencodable = sequence;
    unencodable.events[1].tick = 0x1000'0000U;
    unencodable.events[2].tick = 0x1000'0000U;
    const auto rejected = axk::sequence_to_smf0(unencodable);
    ASSERT_FALSE(rejected);
    EXPECT_NE(rejected.error().message.find("variable-length quantity"), std::string::npos);
}

TEST(CurrentSequence, NormalizesPpqnAndRejectsLossyMidiProfiles) {
    const auto normalized = import_smf(test_smf(480U), "Normalized");
    ASSERT_TRUE(normalized) << normalized.error().message;
    const auto decoded = axk::decode_object(*normalized);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(std::get<axk::CurrentSequence>(decoded->payload).end_tick, 19U);

    const auto sys_exclusive = smf0({std::byte{}, std::byte{0xf0}, std::byte{1}, std::byte{0x7e}, std::byte{},
                                     std::byte{0xff}, std::byte{0x2f}, std::byte{}});
    EXPECT_FALSE(import_smf(sys_exclusive, "SysEx"));

    const auto precise_pitch_bend = smf0({std::byte{}, std::byte{0xe0}, std::byte{1}, std::byte{64}, std::byte{},
                                          std::byte{0xff}, std::byte{0x2f}, std::byte{}});
    EXPECT_FALSE(import_smf(precise_pitch_bend, "Pitch"));

    const auto native_terminator_in_meta =
        smf0({std::byte{}, std::byte{0xff}, std::byte{1}, std::byte{1}, std::byte{0xfd}, std::byte{}, std::byte{0xff},
              std::byte{0x2f}, std::byte{}});
    EXPECT_FALSE(import_smf(native_terminator_in_meta, "Meta"));

    auto format_one = test_smf();
    format_one[9] = std::byte{1};
    EXPECT_FALSE(import_smf(format_one, "Format 1"));

    auto smpte = test_smf();
    smpte[12] = std::byte{0xe7};
    EXPECT_FALSE(import_smf(smpte, "SMPTE"));
    EXPECT_FALSE(import_smf(test_smf(), "Name\x7f"));
}

TEST(CurrentSequence, RejectsMalformedNativeTimeline) {
    const auto generated = import_smf(channel_family_smf(), "Sequence");
    ASSERT_TRUE(generated);

    auto missing_status = *generated;
    missing_status[0x8aU] = std::byte{61};
    const auto missing_status_result = axk::decode_object(missing_status);
    ASSERT_FALSE(missing_status_result);
    EXPECT_NE(missing_status_result.error().message.find("Sequence 'Sequence'"), std::string::npos);
    EXPECT_NE(missing_status_result.error().message.find("object offset 0x8a"), std::string::npos);
    EXPECT_NE(missing_status_result.error().message.find("block 0, event 0, tick 448"), std::string::npos);
    EXPECT_NE(missing_status_result.error().message.find("running status"), std::string::npos);

    auto invalid_size = *generated;
    invalid_size[0x8cU] = std::byte{0xfd};
    const auto invalid_size_result = axk::decode_object(invalid_size);
    ASSERT_FALSE(invalid_size_result);
    EXPECT_NE(invalid_size_result.error().message.find("status 0x90 expects 3 bytes but observed 2"),
              std::string::npos);

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
