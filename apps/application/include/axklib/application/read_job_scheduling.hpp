#pragma once

#include <cstdint>
#include <string>

namespace axk::app {
enum class ReadIntent : std::uint8_t { playback, selected, ordinary, refinement, prefetch };

struct ReadJobScheduling {
    std::string scope;
    ReadIntent intent{ReadIntent::ordinary};
};
} // namespace axk::app
