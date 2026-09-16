#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "image_session_floppy_set.hpp"

namespace axk::app::image_sessions_internal {
struct OpenedDirectorySource {
    OpenedFloppySource opened;
    std::shared_ptr<const RandomAccessReader> snapshot;
};

Result<OpenedDirectorySource> open_directory_source(const Sandbox &sandbox, const ImageSourceRef &source,
                                                    const std::vector<ImageSourceRef> &companions,
                                                    PathReservationCoordinator *reservations,
                                                    const CancellationToken &cancellation);
Result<std::vector<ImageSourceRef>> sibling_directory_sources(const Sandbox &sandbox, const ImageSourceRef &source,
                                                              std::string_view set_label, bool cataloged,
                                                              const CancellationToken &cancellation);
} // namespace axk::app::image_sessions_internal
