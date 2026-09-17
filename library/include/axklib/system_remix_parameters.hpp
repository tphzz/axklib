#pragma once

#include <cstdint>
#include <vector>

namespace axk {

// Relative to floor(primary loop frames / 8), not independently rounded fractions.
enum class SystemRemixDuration : std::uint8_t { eighth = 1, quarter = 2 };
enum class SystemRemixProcessing : std::uint8_t {
    copy = 0,
    random_slice = 1,
    reverse_random_slice = 3,
    fixed_slice = 5,
    silence = 6,
    lo_fi_random_slice = 11,
    pitch_random_slice = 19
};

struct SystemRemixStep {
    SystemRemixDuration duration{SystemRemixDuration::eighth};
    SystemRemixProcessing processing{SystemRemixProcessing::copy};
    // Retained byte; random source selection uses low bits. Must be zero for
    // copy, fixed_slice (source eighth 2) and silence, which do not draw a choice.
    std::uint8_t random_choice{};
};

// Replaces one User1..User5 recipe (zero-based slot). Empty steps reset it to
// unchanged-source copying. Otherwise eighth + 2*quarter counts must total 8.
// Replaced lanes have zero tails. Untouched slots remain byte-exact.
// This is storage editing, not execution or validation of a future source:
// playback requires valid Wave capacity and matching active stereo loop windows.
// Gate and finer subdivisions are not independent authoring options.
struct SystemRegisteredRemixPatch {
    std::uint8_t slot{};
    std::vector<SystemRemixStep> steps;
};

} // namespace axk
