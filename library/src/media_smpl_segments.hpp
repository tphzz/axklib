#pragma once

#include "axklib/media.hpp"

namespace axk::detail {
[[nodiscard]] std::vector<std::byte> smpl_segment_identity(const MediaObject &object);
[[nodiscard]] Result<bool> assemble_smpl_segment_group(std::vector<MediaObject> &objects,
                                                       std::vector<std::size_t> &indices,
                                                       const CancellationToken &cancellation = {});
} // namespace axk::detail
