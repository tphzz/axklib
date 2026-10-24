#include "read_diagnostics.hpp"

#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {
using namespace axk::app;

class ReadSink final : public axk::diagnostic_sink {
  public:
    bool capture{true};
    bool fail{};
    std::vector<axk::diagnostic_event> events;
    bool enabled(axk::diagnostic_level) const noexcept override { return capture; }
    void report(const axk::diagnostic_event &event) override {
        if (fail)
            throw std::runtime_error("diagnostic sink failure");
        events.push_back(event);
    }
};

std::uint64_t metric(const axk::diagnostic_event &event, std::string_view name) {
    for (const auto &value : event.metrics)
        if (value.name == name)
            return value.value;
    return 0U;
}

TEST(ReadDiagnostics, PhasesAndTotalsContainOnlyTheirOwnReads) {
    ReadSink sink;
    {
        ReadDiagnosticScope trace{&sink, "image_preview_read"};
        trace.phase("source-verify");
        ReadDiagnosticScope::add(ReadMetric::binding_checks, 1U);
        trace.phase("pcm-scan");
        ReadDiagnosticScope::record_read(4096U, 2000U, true);
        ReadDiagnosticScope::record_read(32768U, 3000U, false);
        trace.complete();
        EXPECT_TRUE(sink.events.empty());
    }
    ASSERT_EQ(sink.events.size(), 4U);
    EXPECT_EQ(sink.events[1].phase, "source-verify");
    EXPECT_EQ(metric(sink.events[1], "bindingChecks"), 1U);
    EXPECT_EQ(metric(sink.events[1], "transportReads"), 0U);
    EXPECT_EQ(metric(sink.events[2], "transportReads"), 2U);
    EXPECT_EQ(metric(sink.events[2], "transportReadBytes"), 36864U);
    EXPECT_EQ(metric(sink.events[2], "transportReadTimeUs"), 5U);
    EXPECT_EQ(metric(sink.events[2], "readLe4KiB"), 1U);
    EXPECT_EQ(metric(sink.events[2], "readLe64KiB"), 1U);
    EXPECT_EQ(metric(sink.events[2], "failedReads"), 1U);
    EXPECT_EQ(sink.events.back().phase, "total");
    EXPECT_EQ(sink.events.back().outcome, "success");
    EXPECT_EQ(metric(sink.events.back(), "bindingChecks"), 1U);
    EXPECT_EQ(metric(sink.events.back(), "transportReads"), 2U);
}

TEST(ReadDiagnostics, NestedAndConcurrentScopesDoNotStealAttribution) {
    ReadSink outer;
    ReadSink inner;
    ReadSink concurrent;
    {
        ReadDiagnosticScope trace{&outer, "outer"};
        ReadDiagnosticScope::record_read(4096U, 0U, true);
        {
            ReadDiagnosticScope nested{&inner, "inner"};
            ReadDiagnosticScope::record_read(65536U, 0U, true);
        }
        std::thread worker{[&] {
            ReadDiagnosticScope other{&concurrent, "concurrent"};
            ReadDiagnosticScope::record_read(1048576U, 0U, true);
            other.complete();
        }};
        worker.join();
        ReadDiagnosticScope::record_read(4096U, 0U, true);
    }
    EXPECT_EQ(metric(outer.events.back(), "transportReadBytes"), 8192U);
    EXPECT_EQ(metric(inner.events.back(), "transportReadBytes"), 65536U);
    EXPECT_EQ(metric(concurrent.events.back(), "transportReadBytes"), 1048576U);
    EXPECT_EQ(inner.events.back().outcome, "failed");
}

TEST(ReadDiagnostics, DisabledScopesAndBrokenSinksDoNotAffectTheOperation) {
    ReadSink disabled;
    disabled.capture = false;
    {
        ReadDiagnosticScope trace{&disabled, "disabled"};
        EXPECT_FALSE(ReadDiagnosticScope::active());
        trace.phase("pcm-scan");
        ReadDiagnosticScope::record_read(4096U, 1000U, true);
        trace.complete();
    }
    EXPECT_TRUE(disabled.events.empty());
    ReadSink broken;
    broken.fail = true;
    EXPECT_NO_THROW({
        ReadDiagnosticScope trace(&broken, "broken");
        trace.complete();
    });
    EXPECT_FALSE(ReadDiagnosticScope::active());
}
} // namespace
