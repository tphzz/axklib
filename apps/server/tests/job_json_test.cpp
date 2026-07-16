#include <gtest/gtest.h>

#include "axklib/server/job_json.hpp"

namespace {

TEST(ServerJobJson, SerializesStableSnapshotWithoutOwnerOrNativeState) {
    const axk::app::JobSnapshot snapshot{.job_id = "job-one",
                                         .operation_id = "test.operation",
                                         .state = axk::app::JobState::completed,
                                         .latest_sequence = 3U,
                                         .progress = axk::app::JobProgress{"writing", 2U, 2U, "done"},
                                         .result = std::optional<nlohmann::json>{nlohmann::json{{"value", 7}}},
                                         .error = std::nullopt};
    const auto document = axk::server::job_snapshot_json(snapshot);
    EXPECT_EQ(document.at("state"), "COMPLETED");
    EXPECT_EQ(document.at("progress").at("phase"), "writing");
    EXPECT_EQ(document.at("result").at("value"), 7);
    EXPECT_TRUE(document.at("error").is_null());
    EXPECT_FALSE(document.contains("ownerId"));
}

TEST(ServerJobJson, SerializesVersionedEventWithoutOwner) {
    const axk::app::JobEvent event{.event_id = "event-one",
                                   .sequence = 4U,
                                   .job_id = "job-one",
                                   .operation_id = "test.operation",
                                   .owner_id = "private-principal",
                                   .type = "progress",
                                   .state = axk::app::JobState::running,
                                   .timestamp_unix_ms = 123U,
                                   .progress = axk::app::JobProgress{"reading", 1U, 3U, "read"}};
    const auto document = axk::server::job_event_json(event);
    EXPECT_EQ(document.at("schemaVersion"), "1");
    EXPECT_EQ(document.at("state"), "RUNNING");
    EXPECT_EQ(document.at("jobUrl"), "/api/v1/jobs/job-one");
    EXPECT_FALSE(document.contains("ownerId"));
}

} // namespace
