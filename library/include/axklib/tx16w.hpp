#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/export.hpp"
#include "axklib/io.hpp"

namespace axk {

class FatImage;

namespace tx16w {

enum class Profile : std::uint8_t { yamaha_native, yamaha_native_with_auxiliary_files };
enum class SampleRateSource : std::uint8_t { explicit_header, length_markers, defaulted_33333 };
enum class NativeSetupLayout : std::uint8_t { legacy, version_0200 };
enum class ParseNoticeDisposition : std::uint8_t { defaulted, omitted };
enum class ImportMode : std::uint8_t { hierarchy, wave_data_only };

struct ParseNotice {
    ParseNoticeDisposition disposition{ParseNoticeDisposition::omitted};
    std::string source_object;
    std::string source_parameter;
    std::string message;
};

struct Wave {
    std::string name;
    std::uint32_t sample_rate{};
    std::uint32_t attack_frames{};
    std::uint32_t repeat_frames{};
    bool looped{};
    std::vector<std::int16_t> pcm;
    SampleRateSource sample_rate_source{SampleRateSource::explicit_header};
    std::uint8_t native_rate_code{};
    std::uint8_t native_attack_rate_marker{};
    std::uint8_t native_repeat_rate_marker{};
    std::string source_member;
    std::optional<std::uint8_t> native_slot;
};

struct WaveReference {
    std::uint8_t slot{};
    std::string name;
};

struct VoiceRegion {
    std::uint8_t timbre_slot{};
    std::uint8_t low_key_number{};
    std::uint8_t high_key_number{};
    std::uint8_t fade{};
    std::uint8_t native_timbre_selector_flags{};
};

struct Voice {
    std::uint8_t slot{};
    std::string name;
    std::vector<VoiceRegion> regions;
};

struct Timbre {
    std::uint8_t slot{};
    std::string name;
    std::uint8_t wave_slot{};
    std::uint8_t root_key_number{};
};

struct PerformanceVoice {
    std::uint8_t receive_channel{};
    std::uint8_t voice_slot{};
    std::uint8_t alternative_group{};
    std::uint8_t audio_output{};
    std::uint8_t volume{};
    std::int8_t detune{};
    std::int8_t transpose{};
};

struct Performance {
    std::uint8_t slot{};
    std::string name;
    std::array<PerformanceVoice, 16U> voices{};
    std::array<std::uint8_t, 2U> native_header{};
    std::array<std::uint8_t, 6U> native_controls{};
};

struct NativeSetup {
    std::string name;
    std::vector<WaveReference> waves;
    std::vector<Performance> performances;
    std::vector<Voice> voices;
    std::vector<Timbre> timbres;
    NativeSetupLayout layout{NativeSetupLayout::legacy};
    bool write_protected{};
};

struct SourceFile {
    std::string name;
    std::vector<std::byte> bytes;
    std::string source_member;
};

struct Inspection {
    Profile profile{Profile::yamaha_native};
    std::vector<NativeSetup> setups;
    std::vector<Wave> waves;
    std::vector<std::string> unsupported_files;
    std::vector<ParseNotice> notices;
};

[[nodiscard]] AXK_API Result<Wave> decode_wave(std::span<const std::byte> bytes, std::string source_name = {});
[[nodiscard]] AXK_API Result<NativeSetup> decode_native_setup(std::span<const std::byte> setup_bytes,
                                                              std::span<const std::byte> voice_bytes,
                                                              std::span<const std::byte> performance_bytes,
                                                              std::string setup_name = {});
[[nodiscard]] AXK_API Result<Inspection> inspect_files(std::span<const SourceFile> files,
                                                       const CancellationToken &cancellation = {});
[[nodiscard]] AXK_API Result<Inspection> inspect_disk(const FatImage &disk, const CancellationToken &cancellation = {});

} // namespace tx16w
} // namespace axk
