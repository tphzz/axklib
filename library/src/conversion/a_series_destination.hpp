#pragma once

#include "axklib/conversion/plan.hpp"
#include "axklib/formats/a_series/authoring/prepare.hpp"

namespace axk::conversion {
[[nodiscard]] Result<a_series::authoring::Destination> volume_destination(const nlohmann::json &selection);
[[nodiscard]] Result<std::shared_ptr<const PreparedPlan>>
finish_volume_plan(const Request &, const a_series::authoring::Destination &, const a_series::authoring::Bundle &,
                   Report, const CancellationToken &, ProgressSink *);
} // namespace axk::conversion
