#pragma once

#include <cstdint>

#include "axklib/application/image_filesystem.hpp"
#include "axklib/sfs.hpp"

namespace axk::app::detail {
void describe_sfs_attributes(ImageFilesystemEntry &entry, const IndexRecord &record, const Partition &partition,
                             std::uint32_t sector_bytes);
void describe_fat_attributes(ImageFilesystemEntry &entry, std::uint8_t bits);
} // namespace axk::app::detail
