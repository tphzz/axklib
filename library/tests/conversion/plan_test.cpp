#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/conversion/plan.hpp"
#include "axklib/conversion/tx16w/wave_disk_writer.hpp"
#include "axklib/filesystems/fat/view.hpp"
#include "axklib/formats/tx16w/codec.hpp"

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

TEST(ConversionPlan, OnlyExplicitMappingDecisionsCanBeAcknowledged) {
    axk::conversion::Report report;
    report.executable = true;
    report.issues = {
        {"representation", "pcm-expansion", axk::conversion::Impact::representation, {}, {}, {}, {}, "Exact"},
        {"loss", "resampling", axk::conversion::Impact::loss, {}, {}, {}, {}, "Resampled"},
        {"uncertain", "rate", axk::conversion::Impact::uncertainty, {}, {}, {}, {}, "Rate defaulted"}};
    const axk::conversion::PreparedPlan notes{report, {}, 0U, 0U};
    EXPECT_TRUE(notes.acknowledge({}));
    EXPECT_FALSE(notes.acknowledge(std::vector<std::string>{"loss"}));
    EXPECT_FALSE(notes.acknowledge(std::vector<std::string>{"uncertain"}));
    report.issues.push_back({"choice",
                             "missing-performance",
                             axk::conversion::Impact::uncertainty,
                             {},
                             {},
                             {},
                             {},
                             "No Performance",
                             "Create an audition Program"});
    const axk::conversion::PreparedPlan decision{report, {}, 0U, 0U};
    EXPECT_FALSE(decision.acknowledge({}));
    EXPECT_TRUE(decision.acknowledge(std::vector<std::string>{"choice"}));
    EXPECT_FALSE(decision.acknowledge(std::vector<std::string>{"choice", "loss"}));
    EXPECT_FALSE(decision.acknowledge(std::vector<std::string>{"choice", "unknown"}));
    EXPECT_FALSE(decision.acknowledge(std::vector<std::string>{"choice", "choice"}));
    report.executable = false;
    EXPECT_FALSE((axk::conversion::PreparedPlan{report, {}, 0U, 0U}.acknowledge(std::vector<std::string>{"choice"})));
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
    auto invalid_route = profile;
    invalid_route.route = axk::conversion::RouteDescriptor{"Floppy", "fat12", {"WAVE_DATA", "WAVE_DATA"}, {}, 0};
    EXPECT_FALSE(registry.add(invalid_route));
    EXPECT_TRUE(registry.profiles().empty());
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

TEST(ConversionRegistry, AnAlternateWaveReaderComposesWithTheShippingWriterAndPreparesPlayablePcm) {
    using namespace axk::conversion;
    const auto reader = WaveReaderAdapter{
        "test.source",
        "Fixture tone",
        {{"type", "object"}, {"required", {"tone"}}},
        {"fixture-tone"},
        [](const Request &request, const axk::CancellationToken &) -> axk::Result<std::vector<WaveTransfer>> {
            const auto sample = request.options.at("tone").get<std::int16_t>();
            WaveTransfer wave{"fixture-object",
                              "TONE",
                              33333U,
                              0U,
                              128U,
                              WavePlayback::forward_one_shot,
                              0U,
                              0U,
                              60U,
                              0,
                              [sample](std::uint64_t, std::size_t count, const axk::CancellationToken &) {
                                  return std::vector<std::int16_t>(count, sample);
                              }};
            return std::vector<WaveTransfer>{std::move(wave)};
        }};
    for (const bool reader_first : {false, true}) {
        Registry registry;
        if (reader_first) {
            ASSERT_TRUE(registry.add(reader));
            EXPECT_TRUE(registry.profiles().empty());
            ASSERT_TRUE(registry.add(tx16w_wave_disk::writer()));
        } else {
            ASSERT_TRUE(registry.add(tx16w_wave_disk::writer()));
            EXPECT_TRUE(registry.profiles().empty());
            ASSERT_TRUE(registry.add(reader));
        }
        EXPECT_FALSE(registry.add(reader));
        EXPECT_FALSE(registry.add(tx16w_wave_disk::writer()));
        ASSERT_EQ(registry.profiles().size(), 1U);
        const auto &profile = registry.profiles().front();
        ASSERT_EQ(profile.id, "test.source-to-tx16w.wave-disk");
        EXPECT_EQ(profile.options_schema, reader.selection_schema);
        ASSERT_TRUE(profile.route);
        EXPECT_EQ(profile.route->editor_capabilities, (std::vector<std::string>{"fixture-tone", "new-image-file"}));
        Request request;
        request.source = std::make_shared<TestDocument>();
        request.destination.format_id = "yamaha.tx16w";
        request.options = {{"tone", -512}};
        const auto result = registry.prepare(profile.id, request);
        ASSERT_TRUE(result) << result.error().message;
        const auto &plan = **result;
        ASSERT_TRUE(plan.report().executable);
        ASSERT_EQ(plan.report().actions.size(), 1U);
        EXPECT_EQ(plan.report().actions.front().source_object, "fixture-object");
        const auto &disk = std::get<NewImage>(plan.output()).bytes;
        const auto fat = axk::filesystems::open_fat(std::make_shared<axk::MemoryReader>(disk), "disk.img");
        ASSERT_TRUE(fat) << fat.error().message;
        ASSERT_EQ(fat->entries.size(), 1U);
        const auto &entry = fat->entries.front();
        EXPECT_EQ(entry.name, "TONE.W01");
        ASSERT_TRUE(entry.reader);
        std::vector<std::byte> bytes(static_cast<std::size_t>(entry.reader->size()));
        ASSERT_TRUE(entry.reader->read_exact_at(0U, bytes));
        const auto wave = axk::tx16w::decode_wave(bytes);
        ASSERT_TRUE(wave) << wave.error().message;
        EXPECT_EQ(wave->pcm, std::vector<std::int16_t>(128U, -512));
    }
}

TEST(ConversionRegistry, WaveSetPlacesWholeWavesInOrderWithoutRenumberingAcrossFloppies) {
    using namespace axk::conversion;
    Registry registry;
    ASSERT_TRUE(registry.add(WaveReaderAdapter{
        "test.source",
        "Selection",
        {{"type", "object"}},
        {},
        [](const Request &input, const axk::CancellationToken &) -> axk::Result<std::vector<WaveTransfer>> {
            std::vector<WaveTransfer> waves;
            for (const auto &frames : input.options.at("frames")) {
                const auto slot = waves.size() + 1U;
                const auto id = "TONE" + std::to_string(slot);
                waves.push_back({id, id, 33333U, 0U, frames.get<std::uint32_t>(), WavePlayback::forward_one_shot, 0U,
                                 0U, 60U, 0, [slot](std::uint64_t, std::size_t count, const axk::CancellationToken &) {
                                     return std::vector<std::int16_t>(count, static_cast<std::int16_t>(slot * 16U));
                                 }});
            }
            return waves;
        }}));
    ASSERT_TRUE(registry.add(tx16w_wave_disk::writer()));
    ASSERT_TRUE(registry.add(tx16w_wave_disk::set_writer()));
    Request request;
    request.source = std::make_shared<TestDocument>();
    request.destination.format_id = "yamaha.tx16w";
    request.options = {{"frames", {150000U, 150000U, 260000U}}};
    const auto single = registry.prepare("test.source-to-tx16w.wave-disk", request);
    ASSERT_TRUE(single) << single.error().message;
    EXPECT_FALSE((*single)->report().executable);
    EXPECT_TRUE(std::get<NewImage>((*single)->output()).bytes.empty());
    const auto prepare_set = [&] { return registry.prepare("test.source-to-tx16w.wave-disk-set", request); };
    const auto result = prepare_set();
    ASSERT_TRUE(result) << result.error().message;
    const auto &plan = **result;
    ASSERT_TRUE(plan.report().executable);
    ASSERT_EQ(plan.report().actions.size(), 3U);
    const auto &members = std::get<NewImageSet>(plan.output()).members;
    ASSERT_EQ(members.size(), 2U);
    std::size_t slot{};
    for (std::size_t i = 0; i < members.size(); ++i) {
        const auto &member = members[i];
        EXPECT_EQ(member.filename, i == 0U ? "DISK01.IMG" : "DISK02.IMG");
        const auto fat = axk::filesystems::open_fat(std::make_shared<axk::MemoryReader>(member.bytes), member.filename);
        ASSERT_TRUE(fat) << fat.error().message;
        ASSERT_EQ(fat->entries.size(), i == 0U ? 2U : 1U);
        for (const auto &entry : fat->entries) {
            ++slot;
            EXPECT_EQ(entry.name, "TONE" + std::to_string(slot) + ".W0" + std::to_string(slot));
            EXPECT_TRUE(
                std::ranges::any_of(plan.report().actions[slot - 1U].details, [&](const Action::Detail &detail) {
                    return detail.label == "Disk" && detail.value == member.filename;
                }));
            ASSERT_TRUE(entry.reader);
            std::vector<std::byte> bytes(static_cast<std::size_t>(entry.reader->size()));
            ASSERT_TRUE(entry.reader->read_exact_at(0U, bytes));
            const auto decoded = axk::tx16w::decode_wave(bytes);
            ASSERT_TRUE(decoded) << decoded.error().message;
            EXPECT_EQ(decoded->pcm,
                      std::vector<std::int16_t>(slot < 3U ? 150000U : 260000U, static_cast<std::int16_t>(slot * 16U)));
        }
    }
    request.fidelity = Fidelity::strict;
    auto blocked = prepare_set();
    ASSERT_TRUE(blocked) << blocked.error().message;
    EXPECT_FALSE((*blocked)->report().executable);
    EXPECT_TRUE(std::get<NewImageSet>((*blocked)->output()).members.empty());
    request.fidelity = Fidelity::best;
    request.options["frames"] = std::vector<std::uint32_t>(65U, 128U);
    blocked = prepare_set();
    ASSERT_TRUE(blocked) << blocked.error().message;
    EXPECT_FALSE((*blocked)->report().executable);
    EXPECT_TRUE(std::get<NewImageSet>((*blocked)->output()).members.empty());
}
