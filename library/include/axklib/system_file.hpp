#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/export.hpp"
#include "axklib/object.hpp"
#include "axklib/program_parameters.hpp"
#include "axklib/sample_parameters.hpp"
#include "axklib/sampler_model.hpp"
#include "axklib/sfs.hpp"
#include "axklib/system_favorites_parameters.hpp"
#include "axklib/system_file_parameters.hpp"
#include "axklib/system_global_parameters.hpp"
#include "axklib/system_panel_parameters.hpp"
#include "axklib/system_recording_parameters.hpp"

namespace axk {

enum class SystemFileKind : std::uint8_t { a3000_system, a4000_a5000_system2 };

struct SystemMidiAddress {
    MidiPort port{MidiPort::a};
    std::uint8_t channel{1U};

    friend bool operator==(const SystemMidiAddress &, const SystemMidiAddress &) = default;
};

struct SystemProgramPart {
    std::uint8_t part_number{1U};
    SystemMidiAddress midi;
    // Invalid stored values have no projection; the original bytes remain in the file.
    std::optional<std::uint16_t> program_number;
    std::optional<bool> master;

    friend bool operator==(const SystemProgramPart &, const SystemProgramPart &) = default;
};

struct A3000SystemContext {
    std::optional<SystemMidiAddress> basic_receive;
    bool omni{};
    bool program_change_enabled{};

    friend bool operator==(const A3000SystemContext &, const A3000SystemContext &) = default;
};

struct A4000A5000SystemContext {
    std::optional<ProgramMode> saved_program_mode;
    std::optional<SystemMidiAddress> basic_receive;
    bool omni{};
    bool program_change_enabled{};
    std::vector<SystemProgramPart> parts;

    friend bool operator==(const A4000A5000SystemContext &, const A4000A5000SystemContext &) = default;
};

using SystemFileContext = std::variant<A3000SystemContext, A4000A5000SystemContext>;

struct DecodedSystemFile {
    SystemFileKind kind{SystemFileKind::a3000_system};
    std::uint8_t storage_revision{};
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

// Serializes retained sections byte-for-byte, without applying defaults or edits.
// Rejects invalid framing or metadata/context inconsistent with the retained bytes.
// This is not validation of arbitrary parameter changes or fresh-file initialization.
AXK_API Result<std::vector<std::byte>> encode_system_file(const DecodedSystemFile &file);

// Decodes the saved registered Program parameters in SYSTEM/SYSTEM2, independently
// of the current Program assignments. Raw bytes remain in DecodedSystemFile.
// A3000's single A/D route is represented by ad.left; ad.right is absent.
AXK_API Result<ProgramParameters> decode_system_registered_program(const DecodedSystemFile &file);

// Reads the saved registered Sample parameter block without applying it to any
// Sample or inferring member activity from its stored parameter lanes.
AXK_API Result<DecodedSampleParameters> decode_system_registered_sample(const DecodedSystemFile &file);

// Reads stored recording settings and three effects without applying live
// input/key-range dependencies or normalizing invalid values. Raw bytes remain.
AXK_API Result<DecodedSystemRecording> decode_system_recording(const DecodedSystemFile &file);

// Reads global preferences and SYSTEM2 registered Remix lanes without applying
// the settings, interpreting recipes or altering retained bytes. Native Remix
// selections use their own domains and have no registered recipe lanes.
AXK_API Result<DecodedSystemGlobal> decode_system_global(const DecodedSystemFile &file);

// Reads all ordinary effect favorites and retains the complete raw region.
// Does not apply page-entry normalization or interpret dormant rows as effects.
AXK_API Result<DecodedSystemFavorites> decode_system_favorites(const DecodedSystemFile &file);

// Reads established panel preferences and format command selections, retaining
// all 64 bytes without device-load normalization. Does not execute commands or
// provide defaults for fresh files. Generation-specific projections may be absent.
AXK_API Result<DecodedSystemPanel> decode_system_panel(const DecodedSystemFile &file);

// Changes only requested panel preferences in an independent retained record.
// Requires a matching explicit model and consistent framing/context. EndType
// Graph and Selection-page scope are A3000-only; Tree-page scope is A4000/A5000-only.
// Invalid requests are rejected, not clamped. Does not
// modify waveform data or initialize defaults, and performs no disk I/O.
AXK_API Result<DecodedSystemFile> patch_system_panel(const DecodedSystemFile &file, const SystemPanelPatch &patch,
                                                     ASeriesModel model);

// Changes only requested usable favorite nibbles in an independent copy.
// Requires a matching explicit model and consistent retained framing. Invalid
// selections, inactive positions and duplicate effect rows are rejected.
// Does not edit effect parameters, run callbacks or perform disk I/O.
AXK_API Result<DecodedSystemFile> patch_system_favorites(const DecodedSystemFile &file,
                                                         std::span<const SystemEffectFavoritesPatch> patches,
                                                         ASeriesModel model);

// Patches saved global preferences in an independent copy and refreshes the
// context projection. Requires an explicit matching model. Unknown bytes and
// working/recipe state are retained. Mode writes clear Omni; wave-address
// options are exclusive. Unsupported load-sensitive settings are rejected.
// This performs no disk I/O and does not initialize a fresh file.
AXK_API Result<DecodedSystemFile> patch_system_global(const DecodedSystemFile &file,
                                                      const SystemGlobalParameters &patch, ASeriesModel model);

// Patches saved recording preferences, not live recording operations. Absent
// fields are retained except for input/type-dependent normalization. Explicit
// conflicts and inconsistent final key ranges are rejected. Requires a matching
// model and valid retained framing; no source mutation or disk I/O occurs.
AXK_API Result<DecodedSystemFile> patch_system_recording_configuration(const DecodedSystemFile &file,
                                                                       const SystemRecordingParameters &patch,
                                                                       ASeriesModel model);

// Applies optional fields to the three saved recording effects in a new copy.
// Requires a matching explicit target model and a consistent retained record.
// Type changes reset all 16 parameter words before applying explicit overrides;
// same-type patches preserve untouched words. Other settings and opaque bytes
// are preserved. Failure never modifies the input; this performs no disk I/O.
AXK_API Result<DecodedSystemFile> patch_system_recording_effects(const DecodedSystemFile &file,
                                                                 const std::array<ProgramEffectParameters, 3> &patches,
                                                                 ASeriesModel model);

// Validates and applies configuration and effects as one all-or-nothing copy.
// Shares the individual patch contracts above; failure never modifies the input.
AXK_API Result<DecodedSystemFile> patch_system_recording(const DecodedSystemFile &file,
                                                         const SystemRecordingPatch &patch, ASeriesModel model);

// Validates global, recording, favorites and panel groups as one independent copy.
// An invalid request returns no changed record and never mutates the input.
AXK_API Result<DecodedSystemFile> patch_system_file(const DecodedSystemFile &file, const SystemFilePatch &patch,
                                                    ASeriesModel model);

// Resolves a partition-root PRF3/SYSTEM or PRF3/SYSTEM2 path. A missing path is not an error;
// malformed, ambiguous, or dangling paths are rejected instead of guessed.
AXK_API Result<std::optional<SfsId>> locate_system_file_record(const Partition &partition, SystemFileKind kind);

} // namespace axk
