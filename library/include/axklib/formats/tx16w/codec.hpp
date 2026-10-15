#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/export.hpp"
#include "axklib/io.hpp"

namespace axk {

namespace tx16w {

enum class Profile : std::uint8_t { yamaha_native, yamaha_native_with_auxiliary_files };
enum class SampleRateSource : std::uint8_t { explicit_header, length_markers, defaulted_33333 };
enum class NativeSetupLayout : std::uint8_t { legacy, version_0200 };

struct WaveHeader {
    std::string name;
    std::uint32_t sample_rate{};
    std::uint32_t attack_frames{};
    std::uint32_t repeat_frames{};
    bool looped{};
    SampleRateSource sample_rate_source{SampleRateSource::explicit_header};
    std::uint8_t native_rate_code{};
    std::uint8_t native_attack_rate_marker{};
    std::uint8_t native_repeat_rate_marker{};
    std::optional<std::uint8_t> native_slot;

    friend bool operator==(const WaveHeader &, const WaveHeader &) = default;
};

struct Wave : WaveHeader {
    std::vector<std::int16_t> pcm;
    std::string source_member;
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

struct VoiceFile {
    std::vector<Voice> voices;
    std::vector<Timbre> timbres;
};

struct FileIdentity {
    char kind{};
    std::string set_key;
    std::string set_name;
    std::uint8_t native_slot{};
};

[[nodiscard]] AXK_API Result<Wave> decode_wave(std::span<const std::byte> bytes, std::string source_name = {});
[[nodiscard]] AXK_API Result<std::vector<std::int16_t>>
decode_packed_pcm(std::span<const std::byte> bytes, std::size_t first_frame, std::size_t frame_count);
[[nodiscard]] AXK_API Result<WaveHeader> decode_wave_header(std::span<const std::byte> header, std::uint64_t file_size,
                                                            std::string source_name = {});
[[nodiscard]] AXK_API std::optional<FileIdentity> identify_file(std::string_view name);
[[nodiscard]] AXK_API Result<NativeSetup> decode_setup_file(std::span<const std::byte> bytes,
                                                            std::string source_name = {});
[[nodiscard]] AXK_API Result<VoiceFile> decode_voice_file(std::span<const std::byte> bytes,
                                                          std::string source_name = {});
[[nodiscard]] AXK_API Result<std::vector<Performance>> decode_performance_file(std::span<const std::byte> bytes,
                                                                               std::string source_name = {});
[[nodiscard]] AXK_API Result<NativeSetup> decode_native_setup(std::span<const std::byte> setup_bytes,
                                                              std::span<const std::byte> voice_bytes,
                                                              std::span<const std::byte> performance_bytes,
                                                              std::string setup_name = {});

} // namespace tx16w
} // namespace axk
