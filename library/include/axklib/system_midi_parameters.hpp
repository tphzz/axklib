#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace axk {

enum class SystemSysexReceivePort : std::uint8_t { a, b };

// An absent decoded field is invalid or unavailable stored data. An absent
// patch field leaves its byte unchanged. Device number: 0 off, 1..16, 17 all.
struct SystemMidiParameters {
    std::optional<bool> bulk_protect;
    std::optional<bool> aftertouch_disabled;
    std::optional<bool> control_change_disabled;
    std::optional<bool> pitch_bend_disabled;
    std::optional<std::uint8_t> device_number;
    std::optional<SystemSysexReceivePort> sysex_receive_port;

    friend bool operator==(const SystemMidiParameters &, const SystemMidiParameters &) = default;
};

struct DecodedSystemMidi {
    SystemMidiParameters parameters;
    // Eight bytes in SYSTEM, nine in SYSTEM2; includes non-editable state.
    std::vector<std::byte> raw_bytes;

    friend bool operator==(const DecodedSystemMidi &, const DecodedSystemMidi &) = default;
};

} // namespace axk
