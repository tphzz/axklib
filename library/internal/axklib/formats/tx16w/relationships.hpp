#pragma once

#include <span>

#include "axklib/formats/tx16w/document.hpp"

namespace axk::tx16w::detail {

[[nodiscard]] Result<void> build_relationships(std::span<const NativeObject> objects, std::span<const NativeFile> files,
                                               native::Catalog &catalog, const CancellationToken &cancellation);

} // namespace axk::tx16w::detail
