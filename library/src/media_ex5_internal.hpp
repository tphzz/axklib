#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "axklib/media.hpp"
#include "axklib/semantic.hpp"
#include "media_signatures.hpp"

namespace axk::detail {

[[nodiscard]] Result<FatGeometry> read_ex5_geometry(const RandomAccessReader &reader, std::string_view source,
                                                    const CancellationToken &cancellation);
[[nodiscard]] ContentTree ex5_content_tree(const FatImage &image);

} // namespace axk::detail
