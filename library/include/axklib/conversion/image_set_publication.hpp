#pragma once

#include <filesystem>
#include <functional>

#include "axklib/conversion/plan.hpp"
#include "axklib/publication.hpp"

namespace axk::conversion {

// Populate a caller-owned, private, empty staging directory. Never replace a member.
[[nodiscard]] Result<void> stage_image_set(const PreparedPlan &plan, const std::filesystem::path &staging,
                                           const CancellationToken &cancel = {}, ProgressSink *progress = nullptr);

// Publish every reviewed member and manifest together; destination must not exist.
[[nodiscard]] Result<PublicationOutcome> publish_image_set(const PreparedPlan &plan,
                                                           const std::filesystem::path &destination,
                                                           const std::function<Result<void>()> &verify_source,
                                                           const CancellationToken &cancel = {},
                                                           ProgressSink *progress = nullptr);
} // namespace axk::conversion
