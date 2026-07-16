#pragma once

#include <nlohmann/json.hpp>

#include "axklib/application/jobs.hpp"

namespace axk::server {

[[nodiscard]] nlohmann::json job_snapshot_json(const app::JobSnapshot &snapshot);
[[nodiscard]] nlohmann::json job_event_json(const app::JobEvent &event);

} // namespace axk::server
