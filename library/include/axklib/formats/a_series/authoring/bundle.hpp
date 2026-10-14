#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "axklib/writer.hpp"

namespace axk::a_series::authoring {

struct WaveData {
    WaveformSpec spec;
    ImportedAudio audio;
};

// All media is materialized before authoring; this layer never opens converter inputs.
struct Bundle {
    std::vector<WaveData> wave_data;
    std::vector<SampleSpec> samples;
    std::vector<SampleBankSpec> sample_banks;
    std::vector<ProgramSpec> programs;
};

struct Destination {
    std::uint8_t partition_index{};
    std::string volume_name;
};

struct Inventory {
    std::vector<std::uint8_t> occupied_program_slots;
    std::vector<std::string> wave_data_names;
    std::vector<std::string> sample_names;
    std::vector<std::string> sample_bank_names;
    std::vector<std::string> program_names;
};

} // namespace axk::a_series::authoring
