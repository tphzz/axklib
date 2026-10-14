#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "formats/a_series/floppy_set.hpp"

namespace axk::app::formats::a_series {
struct OpenedDirectorySource {
    OpenedFloppySource opened;
    std::shared_ptr<const RandomAccessReader> snapshot;
};

Result<OpenedDirectorySource> open_directory_source(const ImageSourceMember &primary,
                                                    const std::vector<std::shared_ptr<ImageSourceMember>> &companions,
                                                    const CancellationToken &cancellation);
Result<std::vector<ImageSourceRef>> sibling_directory_sources(const Sandbox &sandbox, const ImageSourceRef &source,
                                                              std::string_view set_label, bool cataloged,
                                                              const CancellationToken &cancellation);
} // namespace axk::app::formats::a_series
