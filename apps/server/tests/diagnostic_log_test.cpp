#include "diagnostic_log.hpp"
#include "request_read_diagnostics.hpp"
#include <chrono>
#include <gtest/gtest.h>
#include <string>
#include <vector>

TEST(ReadDiagnostics, RequestAdapterKeepsCorrelationAndRespectsCapture) {
    std::vector<std::string> lines;
    {
        axk::server::detail::DiagnosticLog log{true, std::chrono::steady_clock::now,
                                               [&](const auto &line) { lines.push_back(line); }};
        axk::server::detail::RequestReadDiagnostics sink{log, "request-1", "image-1", "document-1", "object-1"};
        const axk::diagnostic_event event{.level = axk::diagnostic_level::debug,
                                          .source = axk::diagnostic_source::devices,
                                          .event = "image_preview_read",
                                          .phase = "total",
                                          .outcome = "success",
                                          .duration_us = 1500U,
                                          .metrics = {{"transportReads", 8U}}};
        EXPECT_FALSE(sink.enabled(axk::diagnostic_level::debug));
        sink.report(event);
        log.set_capture(axk::diagnostic_level::debug);
        sink.report(event);
        const auto started = sink.serialization_start();
        sink.serialized(started);
    }
    ASSERT_EQ(lines.size(), 2U);
    const auto event = nlohmann::json::parse(lines.front());
    EXPECT_EQ(event.at("requestId"), "request-1");
    EXPECT_EQ(event.at("imageId"), "image-1");
    EXPECT_EQ(event.at("documentId"), "document-1");
    EXPECT_EQ(event.at("objectId"), "object-1");
    EXPECT_EQ(event.at("durationMs"), 1.5);
    EXPECT_EQ(event.at("transportReads"), 8U);
    EXPECT_EQ(nlohmann::json::parse(lines.back()).at("phase"), "serialize");
}

TEST(DiagnosticLog, RuntimeCaptureExpiresAndWriterFailureDoesNotEscape) {
    auto now = std::chrono::steady_clock::time_point{};
    axk::server::detail::DiagnosticLog log{true, [&] { return now; }, [](const auto &) { throw 1; }};
    EXPECT_EQ(log.capture(), axk::diagnostic_level::info);
    log.set_capture(axk::diagnostic_level::trace);
    EXPECT_EQ(log.capture(), axk::diagnostic_level::trace);
    now += std::chrono::seconds{31};
    EXPECT_EQ(log.capture(), axk::diagnostic_level::info);
    EXPECT_NO_THROW(log.submit({{"level", "error"}, {"event", "failure"}}));
}
TEST(DiagnosticLog, DrainsStructuredEventsBeforeDestruction) {
    std::vector<std::string> lines;
    {
        axk::server::detail::DiagnosticLog log{true, std::chrono::steady_clock::now,
                                               [&](const auto &line) { lines.push_back(line); }};
        log.submit({{"level", "info"}, {"source", "objects"}, {"event", "saved"}});
    }
    ASSERT_EQ(lines.size(), 1U);
    const auto event = nlohmann::json::parse(lines.front());
    EXPECT_EQ(event.at("level"), "info");
    EXPECT_EQ(event.at("source"), "objects");
    EXPECT_TRUE(event.at("timestamp").is_number_integer());
}

TEST(DiagnosticLog, DropsOversizedEventsWithAVisibleBoundedSummary) {
    std::vector<std::string> lines;
    {
        axk::server::detail::DiagnosticLog log{true, std::chrono::steady_clock::now,
                                               [&](const auto &line) { lines.push_back(line); }};
        log.submit({{"level", "info"}, {"message", std::string(65536U, 'x')}});
        log.submit({{"level", "warning"}, {"event", "small-event"}});
    }
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_EQ(nlohmann::json::parse(lines.front()).at("event"), "diagnostic_events_dropped");
    EXPECT_EQ(nlohmann::json::parse(lines.front()).at("count"), 1U);
    EXPECT_EQ(nlohmann::json::parse(lines.back()).at("event"), "small-event");
}

TEST(DiagnosticLog, RejectsUndeclaredLevelsWithoutInferringSeverityFromText) {
    std::vector<std::string> lines;
    {
        axk::server::detail::DiagnosticLog log{true, std::chrono::steady_clock::now,
                                               [&](const auto &line) { lines.push_back(line); }};
        log.submit({{"event", "missing-level"}, {"message", "[INFO] filename"}});
        log.submit({{"level", "critical"}, {"event", "invalid-level"}});
        log.submit_line("plain output without a level");
    }
    ASSERT_EQ(lines.size(), 3U);
    for (const auto &line : lines) {
        const auto event = nlohmann::json::parse(line);
        EXPECT_EQ(event.at("level"), "error");
        EXPECT_EQ(event.at("source"), "other");
        EXPECT_EQ(event.at("event"), "invalid_log_record");
        EXPECT_TRUE(event.contains("originalText"));
    }
}

TEST(DiagnosticLog, MalformedOriginalTextIsBoundedAndTheErrorDoesNotRecurse) {
    std::vector<std::string> lines;
    {
        axk::server::detail::DiagnosticLog log{true, std::chrono::steady_clock::now,
                                               [&](const auto &line) { lines.push_back(line); }};
        log.submit_line(std::string(10000U, 'x'));
    }
    ASSERT_EQ(lines.size(), 1U);
    const auto event = nlohmann::json::parse(lines.front());
    EXPECT_EQ(event.at("event"), "invalid_log_record");
    EXPECT_EQ(event.at("level"), "error");
    EXPECT_EQ(event.at("originalText").get_ref<const std::string &>().size(), 4096U);
    EXPECT_TRUE(event.at("truncated").get<bool>());
}
