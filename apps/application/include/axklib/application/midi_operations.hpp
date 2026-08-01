#pragma once

#include "axklib/application/filesystem.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/application/uploads.hpp"

namespace axk::app {

// Binds bounded Standard MIDI File inspection without exposing storage paths or SysEx payloads.
[[nodiscard]] Result<void> bind_midi_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                UploadStore &uploads);

} // namespace axk::app
