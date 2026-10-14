#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/conversion/plan.hpp"

namespace {
class TestDocument final : public axk::native::Document {
  public:
    TestDocument() {
        auto sources = axk::native::SourceSet::create(
            {{"one", "Source", std::make_shared<axk::MemoryReader>(std::vector<std::byte>{}), {}}});
        if (sources)
            sources_ = *sources;
        summary_.format_id = "test.source";
    }
    const axk::native::DocumentSummary &summary() const noexcept override { return summary_; }
    const axk::native::Catalog &catalog() const noexcept override { return catalog_; }
    const std::shared_ptr<const axk::native::SourceSet> &sources() const noexcept override { return sources_; }

  private:
    axk::native::DocumentSummary summary_;
    axk::native::Catalog catalog_;
    std::shared_ptr<const axk::native::SourceSet> sources_;
};
} // namespace

TEST(ConversionPlan, RequiresOnlyMeaningfulAcknowledgements) {
    axk::conversion::Report report;
    report.executable = true;
    report.issues = {
        {"representation", "pcm-expansion", axk::conversion::Impact::representation, {}, {}, {}, {}, "Exact"},
        {"loss", "resampling", axk::conversion::Impact::loss, {}, {}, {}, {}, "Resampled"},
        {"uncertain", "rate", axk::conversion::Impact::uncertainty, {}, {}, {}, {}, "Rate defaulted"}};
    const axk::conversion::PreparedPlan plan{report, {}, 0U, 0U};
    EXPECT_FALSE(plan.acknowledge({}));
    EXPECT_TRUE(plan.acknowledge(std::vector<std::string>{"loss", "uncertain"}));
    EXPECT_FALSE(plan.acknowledge(std::vector<std::string>{"loss", "uncertain", "unknown"}));
    EXPECT_FALSE(plan.acknowledge(std::vector<std::string>{"loss", "uncertain", "loss"}));
    report.issues.resize(1U);
    const axk::conversion::PreparedPlan exact{report, {}, 0U, 0U};
    EXPECT_TRUE(exact.acknowledge({}));
    report.executable = false;
    EXPECT_FALSE((axk::conversion::PreparedPlan{report, {}, 0U, 0U}.acknowledge({})));
}

TEST(ConversionRegistry, DispatchesAnExplicitNonSamplerProfileAndEnforcesLimits) {
    axk::conversion::Registry registry;
    std::size_t called{};
    auto profile = axk::conversion::Profile{
        "test.profile",
        "Test",
        "test.source",
        "test.destination",
        {},
        [&](const axk::conversion::Request &request, const axk::CancellationToken &,
            axk::ProgressSink *) -> axk::Result<std::shared_ptr<const axk::conversion::PreparedPlan>> {
            ++called;
            axk::conversion::Report report;
            report.profile_id = "test.profile";
            report.fidelity = request.fidelity;
            report.executable = true;
            return std::make_shared<const axk::conversion::PreparedPlan>(
                std::move(report), std::vector<axk::conversion::Patch>{}, 0U, 100U);
        }};
    ASSERT_TRUE(registry.add(profile));
    EXPECT_FALSE(registry.add(profile));
    axk::conversion::Request request;
    request.source = std::make_shared<TestDocument>();
    request.destination = {"test.destination", std::make_shared<axk::MemoryReader>(std::vector<std::byte>{}), {}, {}};
    ASSERT_TRUE(registry.prepare("test.profile", request));
    EXPECT_EQ(called, 1U);
    EXPECT_FALSE(registry.prepare("missing", request));
    request.destination.format_id = "wrong";
    EXPECT_FALSE(registry.prepare("test.profile", request));
    EXPECT_EQ(called, 1U);
    request.destination.format_id = "test.destination";
    request.limits.maximum_bytes = 99U;
    EXPECT_FALSE(registry.prepare("test.profile", request));
}
