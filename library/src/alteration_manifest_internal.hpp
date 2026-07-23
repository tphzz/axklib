#pragma once

#include "axklib/alteration.hpp"

namespace axk::detail {

Result<void> validate_alteration_manifest(const AlterationManifest &manifest);

} // namespace axk::detail
