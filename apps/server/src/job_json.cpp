#include "axklib/server/job_json.hpp"

namespace {

using Json = nlohmann::json;

Json progress_json(const std::optional<axk::app::JobProgress> &progress) {
    if (!progress)
        return nullptr;
    return {{"phase", progress->phase},
            {"completed", progress->completed},
            {"total", progress->total ? Json(*progress->total) : Json(nullptr)},
            {"message", progress->message}};
}

Json error_json(const std::optional<axk::app::Error> &error) {
    if (!error)
        return nullptr;
    Json context = Json::object();
    if (error->context.partition_index)
        context["partitionIndex"] = *error->context.partition_index;
    if (error->context.volume_name)
        context["volumeName"] = *error->context.volume_name;
    if (error->context.object_type)
        context["objectType"] = *error->context.object_type;
    if (error->context.object_name)
        context["objectName"] = *error->context.object_name;
    if (error->context.relative_path)
        context["relativePath"] = *error->context.relative_path;
    return {{"code", error->code},
            {"message", error->message},
            {"context", std::move(context)},
            {"retryable", error->retryable}};
}

} // namespace

nlohmann::json axk::server::job_snapshot_json(const app::JobSnapshot &snapshot) {
    return {{"jobId", snapshot.job_id},
            {"operationId", snapshot.operation_id},
            {"state", app::job_state_name(snapshot.state)},
            {"latestSequence", snapshot.latest_sequence},
            {"progress", progress_json(snapshot.progress)},
            {"result", snapshot.result ? *snapshot.result : Json(nullptr)},
            {"error", error_json(snapshot.error)}};
}

nlohmann::json axk::server::job_event_json(const app::JobEvent &event) {
    return {{"schemaVersion", "1"},
            {"eventId", event.event_id},
            {"sequence", event.sequence},
            {"jobId", event.job_id},
            {"operationId", event.operation_id},
            {"type", event.type},
            {"timestampUnixMs", event.timestamp_unix_ms},
            {"state", app::job_state_name(event.state)},
            {"progress", progress_json(event.progress)},
            {"jobUrl", "/api/v1/jobs/" + event.job_id}};
}
