#pragma once
#include "axklib/application/filesystem_edit_operations.hpp"

namespace axk::app {
[[nodiscard]] Result<void> bind_su700_import_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                        UploadStore &uploads, ImageSessionManager &images,
                                                        AlterationJournalStore &journals);
}
