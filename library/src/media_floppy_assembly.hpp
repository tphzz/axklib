#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/media.hpp"

namespace axk::detail {
struct FloppyPendingObject {
    MediaObject object;
    std::uint16_t slot{};
    std::string catalog_path;
};

Result<std::vector<MediaObject>> assemble_floppy_objects(std::vector<FloppyPendingObject> pending,
                                                         FloppySetStatus status, std::size_t maximum_object_bytes,
                                                         std::string_view source_name, MediaObjectReadMode mode);
} // namespace axk::detail
