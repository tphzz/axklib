#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "axklib/application/image_sessions.hpp"
#include "axklib/native/audio.hpp"

namespace axk::app {

// The owning native document outlives every prepared source. Revision changes
// discard its auditions before replacing its immutable format state.
class NativeAudioProvider {
  public:
    virtual ~NativeAudioProvider() = default;
    [[nodiscard]] virtual Result<native::PcmSource> prepare_source(std::string_view object_id,
                                                                   native::PcmReadWindow window) const = 0;
    [[nodiscard]] virtual Result<std::vector<std::byte>>
    read_member_pcm(const native::PcmMember &member, std::uint64_t first_frame, std::size_t frame_count,
                    const CancellationToken &cancellation) const = 0;
};

struct AuditionEntry {
    ImageAudition descriptor;
    std::vector<native::PcmSource> sources;
    std::chrono::steady_clock::time_point last_access;
};

} // namespace axk::app
