#pragma once

#include <functional>
#include <string_view>
#include <vector>

#include "axklib/application/filesystem.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/path_reservations.hpp"
#include "axklib/io.hpp"
#include "axklib/media.hpp"
#include "image_sources.hpp"

namespace axk::app::formats::a_series {

struct OpenedFloppySource {
    MediaContainer media;
    std::vector<ImageSourceRef> companion_sources;
    ImageFloppySetSummary summary;
};

Result<OpenedFloppySource> open_floppy_source(const ImageSourceMember &primary,
                                              const std::vector<std::shared_ptr<ImageSourceMember>> &companions,
                                              const CancellationToken &cancellation);
Result<std::vector<ImageSourceRef>> immediate_sibling_floppy_sources(const Sandbox &sandbox,
                                                                     const ImageSourceRef &source,
                                                                     std::string_view set_label,
                                                                     const CancellationToken &cancellation);

} // namespace axk::app::formats::a_series
