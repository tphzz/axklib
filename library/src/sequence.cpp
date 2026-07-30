#include "axklib/sequence.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <utility>

#include "axklib/bytes.hpp"

namespace axk {
namespace {

constexpr std::size_t current_header_size = 0x80U;
constexpr std::uint16_t current_division = 96U;
constexpr std::size_t maximum_midi_bytes = 16U * 1024U * 1024U;
constexpr std::size_t maximum_events = 1'000'000U;

Error sequence_error(std::string message, ErrorCode code = ErrorCode::object_malformed) {
    return make_error(code, ErrorCategory::object, std::move(message));
}

Result<std::uint32_t> read_vlq(std::span<const std::byte> bytes, std::size_t &position) {
    std::uint32_t value{};
    for (std::size_t index = 0; index < 4U; ++index) {
        if (position >= bytes.size())
            return std::unexpected{sequence_error("MIDI variable-length quantity is truncated")};
        const auto item = std::to_integer<std::uint8_t>(bytes[position++]);
        if (value > (std::numeric_limits<std::uint32_t>::max() >> 7U))
            return std::unexpected{sequence_error("MIDI variable-length quantity overflows")};
        value = (value << 7U) | (item & 0x7fU);
        if ((item & 0x80U) == 0U)
            return value;
    }
    return std::unexpected{sequence_error("MIDI variable-length quantity exceeds four bytes")};
}

void append_be16(std::vector<std::byte> &output, std::uint16_t value) {
    output.push_back(static_cast<std::byte>(value >> 8U));
    output.push_back(static_cast<std::byte>(value));
}

void append_be32(std::vector<std::byte> &output, std::uint32_t value) {
    output.push_back(static_cast<std::byte>(value >> 24U));
    output.push_back(static_cast<std::byte>(value >> 16U));
    output.push_back(static_cast<std::byte>(value >> 8U));
    output.push_back(static_cast<std::byte>(value));
}

void append_vlq(std::vector<std::byte> &output, std::uint32_t value) {
    std::array<std::byte, 4> encoded{};
    std::size_t count{1U};
    encoded.back() = static_cast<std::byte>(value & 0x7fU);
    while ((value >>= 7U) != 0U) {
        encoded[encoded.size() - 1U - count] = static_cast<std::byte>((value & 0x7fU) | 0x80U);
        ++count;
    }
    output.insert(output.end(), encoded.end() - static_cast<std::ptrdiff_t>(count), encoded.end());
}

std::size_t channel_data_size(std::uint8_t status) {
    const auto family = status & 0xf0U;
    return family == 0xc0U || family == 0xd0U ? 1U : 2U;
}

Result<std::vector<std::byte>> native_channel_message(std::span<const std::byte> message) {
    if (message.empty())
        return std::unexpected{sequence_error("native Sequence channel event is empty")};
    const auto status = std::to_integer<std::uint8_t>(message.front());
    if (status < 0x80U || status >= 0xf0U)
        return std::unexpected{sequence_error("native Sequence channel event has an invalid status")};
    if ((status & 0xf0U) == 0xe0U) {
        if (message.size() != 2U)
            return std::unexpected{sequence_error("native Sequence pitch-bend event has an invalid size")};
        return std::vector<std::byte>{message.front(), std::byte{}, message[1]};
    }
    if (message.size() != channel_data_size(status) + 1U)
        return std::unexpected{sequence_error("native Sequence channel event has an invalid size")};
    return std::vector<std::byte>{message.begin(), message.end()};
}

Result<SequenceEvent> decode_native_event(std::span<const std::byte> stored, std::uint32_t tick,
                                          std::optional<std::uint8_t> &running_status) {
    if (stored.empty())
        return std::unexpected{sequence_error("native Sequence event is empty")};
    auto first = std::to_integer<std::uint8_t>(stored.front());
    SequenceEvent result{.tick = tick, .kind = SequenceEventKind::channel, .message = {}};
    if (first < 0x80U) {
        if (!running_status)
            return std::unexpected{sequence_error("native Sequence running status has no channel status")};
        result.kind = SequenceEventKind::channel;
        result.message.push_back(static_cast<std::byte>(*running_status));
        result.message.insert(result.message.end(), stored.begin(), stored.end());
    } else if (first < 0xf0U) {
        running_status = first;
        result.kind = SequenceEventKind::channel;
        result.message.assign(stored.begin(), stored.end());
    } else if (first == 0xffU) {
        running_status.reset();
        result.kind = SequenceEventKind::meta;
        if (stored.size() < 2U)
            return std::unexpected{sequence_error("native Sequence meta event is truncated")};
        if (stored[1] == std::byte{0x2f}) {
            if (stored.size() != 3U || stored[2] != std::byte{})
                return std::unexpected{sequence_error("native Sequence end-of-track event is malformed")};
            result.message = {std::byte{0xff}, std::byte{0x2f}};
        } else {
            result.message.assign(stored.begin(), stored.end());
        }
    } else if (first == 0xf0U || first == 0xf7U) {
        running_status.reset();
        result.kind = SequenceEventKind::system_exclusive;
        result.message.assign(stored.begin(), stored.end());
    } else {
        return std::unexpected{
            sequence_error("native Sequence contains an unsupported system event", ErrorCode::unsupported_profile)};
    }
    if (result.kind == SequenceEventKind::channel) {
        const auto status = std::to_integer<std::uint8_t>(result.message.front());
        const auto expected = (status & 0xf0U) == 0xe0U ? 2U : channel_data_size(status) + 1U;
        if (result.message.size() != expected)
            return std::unexpected{sequence_error("native Sequence channel event has an invalid size")};
        if (std::ranges::any_of(result.message.begin() + 1, result.message.end(),
                                [](std::byte value) { return (std::to_integer<std::uint8_t>(value) & 0x80U) != 0U; })) {
            return std::unexpected{sequence_error("native Sequence channel data byte has its status bit set")};
        }
    }
    return result;
}

Result<std::vector<std::byte>> encode_smf_event(const SequenceEvent &event) {
    if (event.message.empty())
        return std::unexpected{sequence_error("Sequence event is empty")};
    if (event.kind == SequenceEventKind::channel)
        return native_channel_message(event.message);
    if (event.kind == SequenceEventKind::system_exclusive) {
        return std::unexpected{
            sequence_error("native Sequence SysEx conversion is not admitted", ErrorCode::unsupported_profile)};
    }
    if (event.message.size() < 2U || event.message[0] != std::byte{0xff})
        return std::unexpected{sequence_error("native Sequence meta event is malformed")};
    std::vector<std::byte> result{event.message[0], event.message[1]};
    const auto data = std::span<const std::byte>{event.message}.subspan(2U);
    if (event.message[1] == std::byte{0x2f}) {
        if (!data.empty())
            return std::unexpected{sequence_error("native Sequence end-of-track event is malformed")};
        result.push_back(std::byte{});
        return result;
    }
    append_vlq(result, static_cast<std::uint32_t>(data.size()));
    result.insert(result.end(), data.begin(), data.end());
    return result;
}

struct ParsedSmf {
    std::uint16_t division{};
    std::vector<SequenceEvent> events;
};

Result<ParsedSmf> parse_smf0(std::span<const std::byte> smf) {
    if (smf.size() > maximum_midi_bytes)
        return std::unexpected{sequence_error("MIDI file exceeds the supported size", ErrorCode::io_unsupported_size)};
    const ByteReader reader{smf};
    const auto magic = reader.ascii_field(0U, 4U, false);
    const auto header_size = reader.be32(4U);
    const auto format = reader.be16(8U);
    const auto tracks = reader.be16(10U);
    const auto division = reader.be16(12U);
    const auto track_magic = reader.ascii_field(14U, 4U, false);
    const auto track_size = reader.be32(18U);
    if (!magic || !header_size || !format || !tracks || !division || !track_magic || !track_size)
        return std::unexpected{sequence_error("MIDI header is truncated")};
    if (*magic != "MThd" || *header_size != 6U || *format != 0U || *tracks != 1U || *division == 0U ||
        (*division & 0x8000U) != 0U || *track_magic != "MTrk") {
        return std::unexpected{
            sequence_error("MIDI import requires format 0 with one PPQN track", ErrorCode::unsupported_profile)};
    }
    if (*track_size > smf.size() - 22U || *track_size != smf.size() - 22U)
        return std::unexpected{sequence_error("MIDI track size does not match the file")};

    const auto track = smf.subspan(22U, *track_size);
    ParsedSmf result{.division = *division, .events = {}};
    std::size_t position{};
    std::uint64_t absolute_tick{};
    std::optional<std::uint8_t> running_status;
    bool found_end{};
    while (position < track.size()) {
        if (result.events.size() >= maximum_events)
            return std::unexpected{
                sequence_error("MIDI event count exceeds the supported limit", ErrorCode::io_unsupported_size)};
        auto delta = read_vlq(track, position);
        if (!delta)
            return std::unexpected{delta.error()};
        absolute_tick += *delta;
        if (absolute_tick > std::numeric_limits<std::uint32_t>::max())
            return std::unexpected{sequence_error("MIDI absolute tick exceeds the supported range")};
        if (position >= track.size())
            return std::unexpected{sequence_error("MIDI event is truncated")};

        auto status = std::to_integer<std::uint8_t>(track[position]);
        bool explicit_status = (status & 0x80U) != 0U;
        if (explicit_status) {
            ++position;
        } else if (running_status) {
            status = *running_status;
        } else {
            return std::unexpected{sequence_error("MIDI running status has no preceding channel status")};
        }

        SequenceEvent event{
            .tick = static_cast<std::uint32_t>(absolute_tick), .kind = SequenceEventKind::channel, .message = {}};
        if (status < 0xf0U) {
            running_status = status;
            event.kind = SequenceEventKind::channel;
            event.message.push_back(static_cast<std::byte>(status));
            const auto count = channel_data_size(status);
            if (count > track.size() - position)
                return std::unexpected{sequence_error("MIDI channel event is truncated")};
            for (std::size_t index = 0; index < count; ++index) {
                const auto value = std::to_integer<std::uint8_t>(track[position++]);
                if ((value & 0x80U) != 0U)
                    return std::unexpected{sequence_error("MIDI channel data byte has its status bit set")};
                event.message.push_back(static_cast<std::byte>(value));
            }
        } else if (status == 0xffU) {
            running_status.reset();
            event.kind = SequenceEventKind::meta;
            if (position >= track.size())
                return std::unexpected{sequence_error("MIDI meta event type is truncated")};
            event.message = {std::byte{0xff}, track[position++]};
            auto length = read_vlq(track, position);
            if (!length)
                return std::unexpected{length.error()};
            if (*length > track.size() - position)
                return std::unexpected{sequence_error("MIDI meta event data is truncated")};
            event.message.insert(event.message.end(), track.begin() + static_cast<std::ptrdiff_t>(position),
                                 track.begin() + static_cast<std::ptrdiff_t>(position + *length));
            position += *length;
            found_end = event.message[1] == std::byte{0x2f};
            if (found_end && *length != 0U)
                return std::unexpected{sequence_error("MIDI end-of-track event must be empty")};
        } else if (status == 0xf0U || status == 0xf7U) {
            return std::unexpected{sequence_error("MIDI SysEx import is not admitted", ErrorCode::unsupported_profile)};
        } else {
            return std::unexpected{
                sequence_error("MIDI system-common events are not admitted", ErrorCode::unsupported_profile)};
        }
        result.events.push_back(std::move(event));
        if (found_end) {
            if (position != track.size())
                return std::unexpected{sequence_error("MIDI track contains data after end-of-track")};
            break;
        }
        (void)explicit_status;
    }
    if (!found_end)
        return std::unexpected{sequence_error("MIDI track has no end-of-track event")};
    return result;
}

Result<std::uint32_t> scaled_tick(std::uint32_t tick, std::uint16_t source_division) {
    const auto numerator = static_cast<std::uint64_t>(tick) * current_division + source_division / 2U;
    const auto value = numerator / source_division;
    if (value > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected{sequence_error("normalized Sequence tick exceeds the supported range")};
    return static_cast<std::uint32_t>(value);
}

Result<std::vector<std::byte>> native_event(const SequenceEvent &event, std::optional<std::uint8_t> &running_status) {
    if (event.message.empty())
        return std::unexpected{sequence_error("MIDI event is empty")};
    if (event.kind == SequenceEventKind::meta) {
        running_status.reset();
        if (event.message.size() < 2U || event.message[0] != std::byte{0xff})
            return std::unexpected{sequence_error("MIDI meta event is malformed")};
        if (event.message[1] == std::byte{0x2f})
            return std::vector<std::byte>{std::byte{0xff}, std::byte{0x2f}, std::byte{}};
        if (std::ranges::find(event.message.begin() + 2, event.message.end(), std::byte{0xfd}) != event.message.end()) {
            return std::unexpected{sequence_error("MIDI meta data contains the native Sequence terminator",
                                                  ErrorCode::unsupported_profile)};
        }
        return event.message;
    }
    if (event.kind != SequenceEventKind::channel)
        return std::unexpected{sequence_error("MIDI SysEx import is not admitted", ErrorCode::unsupported_profile)};
    const auto status = std::to_integer<std::uint8_t>(event.message[0]);
    if (status < 0x80U || status >= 0xf0U || event.message.size() != channel_data_size(status) + 1U)
        return std::unexpected{sequence_error("MIDI channel event is malformed")};
    std::vector<std::byte> result;
    if (running_status != status)
        result.push_back(event.message[0]);
    if ((status & 0xf0U) == 0xe0U) {
        if (event.message[1] != std::byte{})
            return std::unexpected{
                sequence_error("MIDI pitch bend requires a zero low-resolution byte", ErrorCode::unsupported_profile)};
        result.push_back(event.message[2]);
    } else {
        result.insert(result.end(), event.message.begin() + 1, event.message.end());
    }
    running_status = status;
    return result;
}

std::optional<std::uint16_t> first_tempo_bpm(std::span<const SequenceEvent> events) {
    const auto found = std::ranges::find_if(events, [](const SequenceEvent &event) {
        return event.kind == SequenceEventKind::meta && event.message.size() == 5U &&
               event.message[0] == std::byte{0xff} && event.message[1] == std::byte{0x51};
    });
    if (found == events.end())
        return std::nullopt;
    const auto micros = (std::to_integer<std::uint32_t>(found->message[2]) << 16U) |
                        (std::to_integer<std::uint32_t>(found->message[3]) << 8U) |
                        std::to_integer<std::uint32_t>(found->message[4]);
    if (micros == 0U)
        return std::nullopt;
    return static_cast<std::uint16_t>(std::clamp<std::uint32_t>((60'000'000U + micros / 2U) / micros, 30U, 300U));
}

} // namespace

Result<CurrentSequence> decode_current_sequence(std::span<const std::byte> payload) {
    if (payload.size() < current_header_size + 10U)
        return std::unexpected{sequence_error("current Sequence payload is too short for its timeline")};
    const ByteReader reader{payload};
    const auto version = reader.be16(0x7cU);
    const auto division = reader.be16(0x7eU);
    const auto first_tick = reader.be32(0x80U);
    if (!version || !division || !first_tick)
        return std::unexpected{sequence_error("current Sequence header is truncated")};
    if (*version != 1U || *division != current_division) {
        return std::unexpected{sequence_error("Sequence does not use the admitted current timeline profile",
                                              ErrorCode::unsupported_profile)};
    }

    CurrentSequence result;
    result.raw_payload.assign(payload.begin(), payload.end());
    result.format_version = *version;
    result.ticks_per_quarter_note = *division;
    result.first_tick = *first_tick;
    const auto tempo = reader.be16(0x6cU);
    if (tempo && *tempo >= 30U && *tempo <= 300U)
        result.tempo_bpm = *tempo;

    std::size_t position = 0x84U;
    std::uint32_t tick = *first_tick;
    std::optional<std::uint8_t> running_status;
    bool found_end{};
    while (!found_end) {
        if (position > payload.size() || 6U > payload.size() - position)
            return std::unexpected{sequence_error("current Sequence timeline block is truncated")};
        const auto next_tick = reader.be32(position);
        const auto count = reader.be16(position + 4U);
        if (!next_tick || !count)
            return std::unexpected{sequence_error("current Sequence timeline block header is truncated")};
        position += 6U;
        if (*count == 0U)
            return std::unexpected{sequence_error("current Sequence timeline block has no events")};
        for (std::size_t index = 0; index < *count; ++index) {
            if (result.events.size() >= maximum_events)
                return std::unexpected{
                    sequence_error("Sequence event count exceeds the supported limit", ErrorCode::io_unsupported_size)};
            const auto terminator = std::ranges::find(payload.begin() + static_cast<std::ptrdiff_t>(position),
                                                      payload.end(), std::byte{0xfd});
            if (terminator == payload.end())
                return std::unexpected{sequence_error("current Sequence event terminator is missing")};
            const auto end = static_cast<std::size_t>(terminator - payload.begin());
            auto event = decode_native_event(payload.subspan(position, end - position), tick, running_status);
            if (!event)
                return std::unexpected{event.error()};
            found_end = event->kind == SequenceEventKind::meta && event->message.size() == 2U &&
                        event->message[0] == std::byte{0xff} && event->message[1] == std::byte{0x2f};
            result.events.push_back(std::move(*event));
            position = end + 1U;
            if (found_end && index + 1U != *count)
                return std::unexpected{sequence_error("current Sequence block contains events after end-of-track")};
        }
        result.event_count = result.events.size();
        result.end_tick = tick;
        position = (position + 3U) & ~std::size_t{3U};
        if (position > payload.size())
            return std::unexpected{sequence_error("current Sequence block alignment exceeds the payload")};
        if (found_end) {
            if (*next_tick != 0U)
                return std::unexpected{sequence_error("current Sequence terminal block has a nonzero next tick")};
            break;
        }
        if (*next_tick < tick)
            return std::unexpected{sequence_error("current Sequence ticks are not monotonic")};
        tick = *next_tick;
    }
    return result;
}

Result<std::vector<std::byte>> sequence_to_smf0(const CurrentSequence &sequence) {
    if (sequence.ticks_per_quarter_note == 0U || sequence.events.empty())
        return std::unexpected{sequence_error("Sequence has no decoded current timeline")};
    std::vector<std::byte> track;
    std::uint32_t previous_tick{};
    bool found_end{};
    for (const auto &event : sequence.events) {
        if (found_end || event.tick < previous_tick)
            return std::unexpected{sequence_error("Sequence events are not in canonical tick order")};
        append_vlq(track, event.tick - previous_tick);
        auto encoded = encode_smf_event(event);
        if (!encoded)
            return std::unexpected{encoded.error()};
        track.insert(track.end(), encoded->begin(), encoded->end());
        previous_tick = event.tick;
        found_end = event.kind == SequenceEventKind::meta && event.message.size() == 2U &&
                    event.message[0] == std::byte{0xff} && event.message[1] == std::byte{0x2f};
    }
    if (!found_end)
        return std::unexpected{sequence_error("Sequence has no end-of-track event")};
    std::vector<std::byte> result;
    result.insert(result.end(), {std::byte{'M'}, std::byte{'T'}, std::byte{'h'}, std::byte{'d'}});
    append_be32(result, 6U);
    append_be16(result, 0U);
    append_be16(result, 1U);
    append_be16(result, sequence.ticks_per_quarter_note);
    result.insert(result.end(), {std::byte{'M'}, std::byte{'T'}, std::byte{'r'}, std::byte{'k'}});
    append_be32(result, static_cast<std::uint32_t>(track.size()));
    result.insert(result.end(), track.begin(), track.end());
    return result;
}

Result<std::vector<std::byte>> smf0_to_current_sequence(std::span<const std::byte> smf,
                                                        std::string_view sequence_name) {
    if (sequence_name.empty() || sequence_name.size() > 16U ||
        !std::ranges::all_of(sequence_name, [](unsigned char value) { return value >= 0x20U && value < 0x7fU; })) {
        return std::unexpected{
            sequence_error("Sequence name must contain 1 to 16 printable ASCII bytes", ErrorCode::invalid_argument)};
    }
    auto parsed = parse_smf0(smf);
    if (!parsed)
        return std::unexpected{parsed.error()};
    for (auto &event : parsed->events) {
        auto tick = scaled_tick(event.tick, parsed->division);
        if (!tick)
            return std::unexpected{tick.error()};
        event.tick = *tick;
    }

    std::map<std::uint32_t, std::vector<std::vector<std::byte>>> groups;
    std::optional<std::uint8_t> running_status;
    for (const auto &event : parsed->events) {
        auto encoded = native_event(event, running_status);
        if (!encoded)
            return std::unexpected{encoded.error()};
        groups[event.tick].push_back(std::move(*encoded));
    }
    if (groups.empty())
        return std::unexpected{sequence_error("MIDI track contains no events")};

    std::vector<std::byte> timeline;
    append_be32(timeline, groups.begin()->first);
    for (auto iterator = groups.begin(); iterator != groups.end(); ++iterator) {
        const auto next = std::next(iterator);
        append_be32(timeline, next == groups.end() ? 0U : next->first);
        if (iterator->second.size() > std::numeric_limits<std::uint16_t>::max())
            return std::unexpected{
                sequence_error("too many MIDI events share one Sequence tick", ErrorCode::io_unsupported_size)};
        append_be16(timeline, static_cast<std::uint16_t>(iterator->second.size()));
        for (const auto &event : iterator->second) {
            timeline.insert(timeline.end(), event.begin(), event.end());
            timeline.push_back(std::byte{0xfd});
        }
        while ((timeline.size() & 3U) != 0U)
            timeline.push_back(std::byte{});
    }

    std::vector<std::byte> result(current_header_size, std::byte{});
    result.insert(result.end(), timeline.begin(), timeline.end());
    ByteWriter writer{result};
    const auto tempo = first_tempo_bpm(parsed->events).value_or(120U);
    if (!writer.write_ascii_field(0U, 12U, "FSFSDEV3SPLX", std::byte{}) ||
        !writer.write_ascii_field(0x0cU, 4U, "SEQU", std::byte{}) || !writer.write_be32(0x14U, 2U) ||
        !writer.write_be32(0x18U, static_cast<std::uint32_t>(result.size() - 0x30U)) ||
        !writer.write_u8(0x30U, 0x13U) || !writer.write_u8(0x31U, 0x0cU) ||
        !writer.write_ascii_field(0x32U, 16U, sequence_name, std::byte{' '}) ||
        !writer.write_ascii_field(0x54U, 16U, sequence_name, std::byte{' '}) || !writer.write_u8(0x64U, 0x30U) ||
        !writer.write_be16(0x6cU, tempo) || !writer.write_be16(0x78U, tempo) || !writer.write_be16(0x7cU, 1U) ||
        !writer.write_be16(0x7eU, current_division)) {
        return std::unexpected{sequence_error("failed to construct the current Sequence header")};
    }
    return result;
}

} // namespace axk
