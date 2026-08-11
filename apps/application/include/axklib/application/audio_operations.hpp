#pragma once

#include "axklib/application/filesystem.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/application/uploads.hpp"

namespace axk::app {

// Binds sampler-audio inspection without exposing transport storage paths.
[[nodiscard]] Result<void> bind_audio_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                 UploadStore &uploads);

} // namespace axk::app
