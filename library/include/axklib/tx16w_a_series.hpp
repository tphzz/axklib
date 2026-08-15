#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/export.hpp"
#include "axklib/tx16w.hpp"
#include "axklib/writer.hpp"

namespace axk::tx16w::a_series {

enum class MappingDisposition : std::uint8_t { exact, approximated, defaulted, omitted, blocked };

struct MappingNotice {
    MappingDisposition disposition{MappingDisposition::exact};
    std::string source_object;
    std::string source_parameter;
    std::string target_object;
    std::string target_parameter;
    std::string message;
};

struct WaveDataPlan {
    std::string name;
    std::size_t source_wave_index{};
    std::uint32_t target_sample_rate{};
    std::uint8_t root_key{};
    std::uint32_t loop_start_frame{};
    std::uint32_t loop_length_frames{};
    AudioSamplerLoopMode loop_mode{AudioSamplerLoopMode::forward_one_shot};
};

struct ImportPlan {
    std::vector<WaveDataPlan> wave_data;
    std::vector<SampleSpec> samples;
    std::vector<SampleBankSpec> sample_banks;
    std::vector<ProgramSpec> programs;
    std::vector<MappingNotice> notices;
};

struct TargetInventory {
    std::vector<std::uint8_t> occupied_program_slots;
    std::vector<std::string> wave_data_names;
    std::vector<std::string> sample_names;
    std::vector<std::string> sample_bank_names;
    std::vector<std::string> program_names;
};

[[nodiscard]] AXK_API Result<ImportPlan> plan_import(const Inspection &inspection, const TargetInventory &target = {},
                                                     ImportMode mode = ImportMode::hierarchy);

} // namespace axk::tx16w::a_series
