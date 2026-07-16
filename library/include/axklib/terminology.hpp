#pragma once

#include <string_view>

namespace axk {

inline constexpr std::string_view sample_structure_category_id{"category:sample-banks-samples"};
inline constexpr std::string_view sample_structure_category_label{"Sample Banks/Samples (SBAC/SBNK)"};
inline constexpr std::string_view sample_structure_selector_component{"Sample Banks and Samples"};
inline constexpr std::string_view wave_data_category_id{"category:wave-data"};
inline constexpr std::string_view wave_data_category_label{"Wave Data (SMPL)"};
inline constexpr std::string_view wave_data_selector_component{"Wave Data"};

[[nodiscard]] constexpr std::string_view sampler_object_label(std::string_view raw_type) {
    if (raw_type == "PROG")
        return "Program (PROG)";
    if (raw_type == "SBAC")
        return "Sample Bank (SBAC)";
    if (raw_type == "SBNK")
        return "Sample (SBNK)";
    if (raw_type == "SMPL")
        return "Wave Data (SMPL)";
    if (raw_type == "SEQU")
        return "Sequence (SEQU)";
    if (raw_type == "PRF3")
        return "Profile (PRF3)";
    return {};
}

[[nodiscard]] constexpr std::string_view sampler_object_tree_label(std::string_view raw_type) {
    if (raw_type == "PROG")
        return "PROGRAM (PROG)";
    if (raw_type == "SBAC")
        return "SAMPLE BANK (SBAC)";
    if (raw_type == "SBNK")
        return "SAMPLE (SBNK)";
    if (raw_type == "SMPL")
        return "WAVE DATA (SMPL)";
    if (raw_type == "SEQU")
        return "SEQUENCE (SEQU)";
    if (raw_type == "PRF3")
        return "PROFILE (PRF3)";
    return {};
}

} // namespace axk
