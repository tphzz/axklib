#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "axklib/program_assignment_parameters.hpp"
#include "axklib/program_parameters.hpp"
#include "axklib/sample_parameters.hpp"
#include "axklib/sample_storage.hpp"
#include "axklib/sampler_model.hpp"

namespace axk {

inline constexpr std::size_t maximum_program_assignments = 999U;
inline constexpr std::array<std::uint32_t, 12> supported_sampler_sample_rates{
    4'000U, 5'512U, 6'000U, 8'000U, 11'025U, 12'000U, 16'000U, 22'050U, 24'000U, 32'000U, 44'100U, 48'000U};

struct WaveDataParameters {
    std::optional<std::uint8_t> root_key;
    std::optional<std::int8_t> fine_tune_cents;
    std::optional<AudioSamplerLoopMode> loop_mode;
    std::optional<std::uint32_t> wave_start_frame;
    std::optional<std::uint32_t> wave_length_frames;
    std::optional<std::uint32_t> loop_start_frame;
    std::optional<std::uint32_t> loop_length_frames;
};

struct WaveformSpec {
    std::string id;
    std::string name;
    std::filesystem::path path;
    std::uint8_t root_key{};
    std::optional<std::uint32_t> target_sample_rate;
    std::int8_t fine_tune_cents{};
    AudioSamplerLoopMode loop_mode{AudioSamplerLoopMode::forward_one_shot};
    std::uint32_t loop_start_frame{};
    std::uint32_t loop_length_frames{};
};

struct SamplePlaybackWindow {
    std::uint32_t start_frame{};
    std::uint32_t length_frames{};
};

struct SampleSpec {
    std::string name;
    std::optional<std::string> waveform_id;
    std::optional<std::string> right_waveform_id;
    std::optional<std::filesystem::path> interleaved_audio_path;
    std::optional<std::string> left_waveform_name;
    std::optional<std::string> right_waveform_name;
    std::optional<std::uint32_t> target_sample_rate;
    SampleParameters parameters;
    std::optional<SamplePlaybackWindow> playback_window{};
    SampleStorageFormat storage_format{SampleStorageFormat::a4000_a5000_224};
};

struct SampleBankSpec {
    std::string name;
    std::vector<std::string> member_samples;
    std::optional<SampleParameters> parameter_overrides{};
    SampleStorageFormat storage_format{SampleStorageFormat::a4000_a5000_224};
};

struct ProgramAssignmentSpec {
    std::string target_kind;
    std::string target_name;
    ProgramAssignmentParameters parameters{};
};

struct ProgramSpec {
    std::uint8_t number{};
    std::string name;
    std::vector<ProgramAssignmentSpec> assignments;
    ASeriesModel model{ASeriesModel::a4000};
    ProgramParameters parameters{};
};

} // namespace axk
