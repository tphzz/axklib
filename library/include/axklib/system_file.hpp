#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/export.hpp"
#include "axklib/object.hpp"
#include "axklib/sfs.hpp"

namespace axk {

enum class SystemFileKind : std::uint8_t { a3000_system, a4000_a5000_system2 };

enum class ASeriesModel : std::uint8_t { a3000, a4000, a5000 };

enum class ProgramMode : std::uint8_t { single, multi };

enum class MidiPort : std::uint8_t { a, b };

struct SystemMidiAddress {
    MidiPort port{MidiPort::a};
    std::uint8_t channel{1U};

    friend bool operator==(const SystemMidiAddress &, const SystemMidiAddress &) = default;
};

struct SystemProgramPart {
    std::uint8_t part_number{1U};
    SystemMidiAddress midi;
    std::uint16_t program_number{1U};
    bool master{};

    friend bool operator==(const SystemProgramPart &, const SystemProgramPart &) = default;
};

struct A3000SystemContext {
    SystemMidiAddress basic_receive;
    bool omni{};
    bool program_change_enabled{};

    friend bool operator==(const A3000SystemContext &, const A3000SystemContext &) = default;
};

struct A4000A5000SystemContext {
    ProgramMode saved_program_mode{ProgramMode::single};
    SystemMidiAddress basic_receive;
    bool omni{};
    bool program_change_enabled{};
    std::vector<SystemProgramPart> parts;

    friend bool operator==(const A4000A5000SystemContext &, const A4000A5000SystemContext &) = default;
};

using SystemFileContext = std::variant<A3000SystemContext, A4000A5000SystemContext>;

struct DecodedSystemFile {
    SystemFileKind kind{SystemFileKind::a3000_system};
    ASeriesModel model{ASeriesModel::a3000};
    CurrentRecordEnvelope record_envelope;
    std::vector<std::byte> system_header_bytes;
    std::vector<std::byte> system_bulk_bytes;
    std::vector<std::byte> reserved_tail_bytes;
    SystemFileContext context;

    friend bool operator==(const DecodedSystemFile &, const DecodedSystemFile &) = default;
};

[[nodiscard]] AXK_API std::size_t system_file_record_size(SystemFileKind kind) noexcept;

// Decodes the complete logical SFS record saved by the sampler's System File command.
// The shared current-record envelope and all unknown/reserved bytes are retained.
AXK_API Result<DecodedSystemFile> decode_system_file(SystemFileKind kind, std::span<const std::byte> payload);

// Resolves a partition-root PRF3/SYSTEM or PRF3/SYSTEM2 path. A missing path is not an error;
// malformed, ambiguous, or dangling paths are rejected instead of guessed.
AXK_API Result<std::optional<SfsId>> locate_system_file_record(const Partition &partition, SystemFileKind kind);

} // namespace axk
