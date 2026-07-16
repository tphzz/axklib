#include <chrono>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/server/telemetry.hpp"

TEST(ServerTelemetry, CountsActiveAndCompletedRequestsByStatusClass) {
    axk::server::RequestTelemetry telemetry;

    telemetry.begin_request();
    telemetry.begin_request();
    telemetry.complete_request(200, std::chrono::milliseconds{7});
    telemetry.complete_request(404, std::chrono::milliseconds{11});

    const auto snapshot = telemetry.snapshot();
    EXPECT_EQ(snapshot.total_requests, 2U);
    EXPECT_EQ(snapshot.active_requests, 0U);
    EXPECT_EQ(snapshot.responses_2xx, 1U);
    EXPECT_EQ(snapshot.responses_4xx, 1U);
    EXPECT_EQ(snapshot.responses_5xx, 0U);
    EXPECT_EQ(snapshot.total_duration_ms, 18U);
}

TEST(ServerTelemetry, StructuredLogContainsOnlyBoundedTransportMetadata) {
    const auto line = axk::server::structured_request_log("request-7", "POST", "/api/v1/files/metadata", 422,
                                                          std::chrono::milliseconds{9});
    const auto parsed = nlohmann::json::parse(line);

    EXPECT_EQ(parsed.at("event"), "http_request");
    EXPECT_EQ(parsed.at("requestId"), "request-7");
    EXPECT_EQ(parsed.at("method"), "POST");
    EXPECT_EQ(parsed.at("path"), "/api/v1/files/metadata");
    EXPECT_EQ(parsed.at("status"), 422);
    EXPECT_EQ(parsed.at("durationMs"), 9);
    EXPECT_EQ(parsed.size(), 6U);
    EXPECT_EQ(line.find("Authorization"), std::string::npos);
    EXPECT_EQ(line.find("relativePath"), std::string::npos);
    EXPECT_EQ(line.find('?'), std::string::npos);
}

TEST(ServerTelemetry, StructuredAuditLogContainsOnlyAllowlistedIdentifiers) {
    const auto line = axk::server::structured_audit_log("request-8", "upload_materialize", "allowed", "desktop",
                                                        "upload", "0123456789abcdef");
    const auto parsed = nlohmann::json::parse(line);

    EXPECT_EQ(parsed.at("event"), "security_audit");
    EXPECT_EQ(parsed.at("requestId"), "request-8");
    EXPECT_EQ(parsed.at("action"), "upload_materialize");
    EXPECT_EQ(parsed.at("outcome"), "allowed");
    EXPECT_EQ(parsed.at("principalId"), "desktop");
    EXPECT_EQ(parsed.at("resourceType"), "upload");
    EXPECT_EQ(parsed.at("resourceId"), "0123456789abcdef");
    EXPECT_EQ(parsed.size(), 7U);
    EXPECT_EQ(line.find("Authorization"), std::string::npos);
    EXPECT_EQ(line.find("relativePath"), std::string::npos);
    EXPECT_EQ(line.find("private-root/"), std::string::npos);
}
