#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace axk::native {

enum class PcmReadWindow : std::uint8_t { stored_pcm, playback };

// All source positions are frames in stored PCM; range ends are start + length, exclusive.
// physical_first_frame selects the window delivered by a bounded frame reader.
struct PcmMember {
    std::string object_id;
    std::string role;
    std::uint16_t output_width{};
    std::uint16_t native_bit_depth{};
    std::uint32_t sample_rate{};
    bool sample_rate_approximate{};
    std::uint64_t stored_frame_count{};
    std::uint64_t playback_start_frame{};
    std::uint64_t playback_length_frames{};
    std::uint64_t source_loop_start{};
    std::uint64_t source_loop_length{};
    std::uint64_t physical_first_frame{};
    std::uint64_t frame_count{};
    std::uint64_t loop_start{};
    std::uint64_t loop_length{};
};

struct PcmSource {
    std::vector<PcmMember> members;
    bool reverse{};
    bool looping{};
    std::vector<std::string> warnings;
};

} // namespace axk::native
