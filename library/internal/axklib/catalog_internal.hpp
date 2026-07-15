#pragma once

#include "axklib/catalog.hpp"

namespace axk::detail {

Result<ObjectCatalog> build_object_catalog(const Container &container, std::size_t maximum_object_bytes,
                                           const CancellationToken &cancellation, bool retain_raw_payloads);

} // namespace axk::detail
