#pragma once

#include <functional>
#include <string_view>
#include <vector>

#include "axklib/application/filesystem.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/path_reservations.hpp"
#include "axklib/io.hpp"
#include "axklib/media.hpp"

namespace axk::app::image_sessions_internal {

struct OpenedFloppySource {
    MediaContainer media;
    std::vector<ImageSourceRef> companion_sources;
    ImageFloppySetSummary summary;
    std::function<Result<void>()> verify_unchanged;
    PathReservationCoordinator::Lease companion_path_lease;
};

Result<OpenedFloppySource> open_floppy_source(const Sandbox &sandbox, const ImageSourceRef &source,
                                              const SandboxFile &primary, MediaContainer media,
                                              const std::vector<ImageSourceRef> &companion_sources,
                                              PathReservationCoordinator *path_reservations,
                                              const CancellationToken &cancellation);
Result<std::vector<ImageSourceRef>> immediate_sibling_floppy_sources(const Sandbox &sandbox,
                                                                     const ImageSourceRef &source,
                                                                     std::string_view set_label,
                                                                     const CancellationToken &cancellation);

} // namespace axk::app::image_sessions_internal

using axk::app::image_sessions_internal::immediate_sibling_floppy_sources;
using axk::app::image_sessions_internal::open_floppy_source;
