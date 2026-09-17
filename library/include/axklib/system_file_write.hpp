#pragma once

#include <filesystem>

#include "axklib/error.hpp"
#include "axklib/export.hpp"
#include "axklib/io.hpp"
#include "axklib/publication.hpp"
#include "axklib/sampler_model.hpp"
#include "axklib/system_file_parameters.hpp"
#include "axklib/types.hpp"

namespace axk {

// Patches an existing, uniquely linked partition System File and publishes a
// new image. Missing files are not created. Allocation and other bytes remain
// unchanged; an existing destination is never replaced. Callers keep the source
// immutable and coordinate shared-image path leases for the entire operation.
// All requested parameter groups are validated together before any image is written.
[[nodiscard]] AXK_API Result<PublicationOutcome>
write_system_file(const std::filesystem::path &source, const std::filesystem::path &destination,
                  PartitionIndex partition, const SystemFilePatch &patch, ASeriesModel model,
                  const CancellationToken &cancellation = {}, ProgressSink *progress = nullptr);

} // namespace axk
