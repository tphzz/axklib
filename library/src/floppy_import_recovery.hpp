#pragma once

#include <map>

#include "axklib/floppy_import.hpp"

namespace axk::detail {
using FloppyFileSources = std::map<std::string, std::vector<FloppyImportFileSource>, std::less<>>;
struct RecoveredFloppyObjects {
    ObjectCatalog catalog;
    RelationshipGraph relationships;
    FloppyFileSources source_files;
    bool joined{};
};
[[nodiscard]] Result<RecoveredFloppyObjects> recover_floppy_objects(std::vector<MediaObject> objects,
                                                                    FloppyFileSources source_files,
                                                                    const CancellationToken &cancellation);
} // namespace axk::detail
