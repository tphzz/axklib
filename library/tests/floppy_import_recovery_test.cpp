#include <array>
#include <map>

#include "axklib/floppy_import.hpp"
#include "axklib/writer_internal.hpp"
#include "media_test_fixtures.hpp"

namespace {
axk::AxkObjectDirectoryEntry entry(std::string path, std::vector<std::byte> bytes) {
    return {std::move(path), std::make_shared<axk::MemoryReader>(std::move(bytes))};
}
std::vector<std::byte> sample(std::string name, std::string wave = "TEST") {
    axk::SampleSpec spec;
    spec.name = std::move(name);
    spec.parameters.root_key = 60U;
    spec.parameters.key_high = 127U;
    const auto bytes = axk::detail::prepare_sbnk_payload(spec, {std::move(wave), 0U, 32000U, 2U});
    EXPECT_TRUE(bytes) << bytes.error().message;
    return bytes.value();
}
std::vector<std::byte> bank() {
    std::map<std::string, axk::SampleSpec> samples;
    for (const auto name : {"Good", "Bad"}) {
        axk::SampleSpec spec;
        spec.name = name;
        samples.emplace(name, spec);
    }
    const auto bytes = axk::detail::prepare_sbac_payload({"Bank", {"Good", "Bad"}}, samples);
    EXPECT_TRUE(bytes) << bytes.error().message;
    return bytes.value();
}
const axk::FloppyImportObject &named(const axk::FloppyImportInspection &inspection, std::string_view name) {
    const auto found = std::ranges::find(inspection.objects, name, &axk::FloppyImportObject::display_name);
    EXPECT_NE(found, inspection.objects.end());
    return *found;
}
std::vector<std::byte> segment(bool first) {
    auto bytes = smpl_object();
    if (first)
        bytes.resize(0xaeU);
    else
        bytes.erase(bytes.begin() + 0xac, bytes.begin() + 0xae);
    be32(bytes, 0x20U, 2U);
    be32(bytes, 0x24U, first ? 0U : 2U);
    return bytes;
}
} // namespace

TEST(FloppyRecovery, ResolvesOnlyUnambiguousAuthoritativeDependenciesAcrossExplicitSources) {
    const auto opened = axk::FloppyImportSource::open_directories(
        {{"samples", {entry("SAMPLE.001", sample("Sample"))}}, {"waves", {entry("WAVE.001", smpl_object())}}});
    ASSERT_TRUE(opened) << opened.error().message;
    const auto &inspection = opened->inspection();
    EXPECT_TRUE(inspection.can_import);
    EXPECT_TRUE(inspection.recovery_used);
    EXPECT_TRUE(inspection.requires_acknowledgement);
    const auto &root = named(inspection, "Sample");
    EXPECT_TRUE(root.exclusion_reason.empty());
    const auto prepared = opened->prepare(std::array{root.key});
    ASSERT_TRUE(prepared) << prepared.error().message;
    EXPECT_EQ(prepared->nodes.size(), 2U);
    EXPECT_TRUE(axk::verify_portable_package(*prepared));
}

TEST(FloppyRecovery, PreservesLocalTargetsAndDoesNotRepairAnUnreadableLocalTargetFromAnotherDisk) {
    auto other = smpl_object();
    other[0xacU] = std::byte{0x7f};
    const auto opened = axk::FloppyImportSource::open_directories(
        {{"local", {entry("SAMPLE.001", sample("Sample")), entry("WAVE.002", smpl_object())}},
         {"other", {entry("WAVE.001", other)}}});
    ASSERT_TRUE(opened) << opened.error().message;
    const auto &root = named(opened->inspection(), "Sample");
    const auto prepared = opened->prepare(std::array{root.key});
    ASSERT_TRUE(prepared) << prepared.error().message;
    const auto wave = std::ranges::find(prepared->nodes, std::string{"SMPL"}, &axk::PackageNode::object_type);
    ASSERT_NE(wave, prepared->nodes.end());
    EXPECT_EQ(wave->raw_payload, smpl_object());
    const auto broken = axk::FloppyImportSource::open_directories(
        {{"local", {entry("SAMPLE.001", sample("Sample")), entry("WAVE.002", segment(true))}},
         {"other", {entry("WAVE.001", smpl_object())}}});
    ASSERT_TRUE(broken) << broken.error().message;
    EXPECT_FALSE(named(broken->inspection(), "Sample").exclusion_reason.empty());
    EXPECT_FALSE(broken->prepare(std::array{named(broken->inspection(), "Sample").key}));
}

TEST(FloppyRecovery, DoesNotChooseAmongAmbiguousSelectedTargets) {
    auto other = smpl_object();
    other[0xacU] = std::byte{0x7f};
    const auto opened = axk::FloppyImportSource::open_directories({{"samples", {entry("SAMPLE.001", sample("Sample"))}},
                                                                   {"one", {entry("WAVE.001", smpl_object())}},
                                                                   {"two", {entry("WAVE.001", other)}}});
    ASSERT_TRUE(opened) << opened.error().message;
    const auto &root = named(opened->inspection(), "Sample");
    EXPECT_FALSE(root.exclusion_reason.empty());
    EXPECT_FALSE(opened->prepare(std::array{root.key}));
}

TEST(FloppyRecovery, ExcludesBrokenClosureWithoutPruningBankMembersOrProgramAssignments) {
    const auto program = axk::detail::prepare_prog_payload({1U, "Program", {{"SBAC", "Bank"}}});
    ASSERT_TRUE(program) << program.error().message;
    const auto opened = axk::FloppyImportSource::open_directories(
        {{"disk",
          {entry("WAVE.001", smpl_object()), entry("GOOD.002", sample("Good")),
           entry("BAD.003", sample("Bad", "Missing")), entry("BANK.004", bank()), entry("PROGRAM.005", *program)}}});
    ASSERT_TRUE(opened) << opened.error().message;
    const auto &inspection = opened->inspection();
    EXPECT_TRUE(named(inspection, "Good").exclusion_reason.empty());
    EXPECT_TRUE(opened->prepare(std::array{named(inspection, "Good").key}));
    for (const auto name : {"Bad", "Bank", "Program"}) {
        const auto &root = named(inspection, name);
        EXPECT_FALSE(root.exclusion_reason.empty()) << name;
        EXPECT_FALSE(opened->prepare(std::array{root.key}));
    }
    EXPECT_TRUE(inspection.requires_acknowledgement);
}

TEST(FloppyRecovery, PreservesSegmentSourcesAndAcceptsOnlyIdenticalDuplicates) {
    const auto opened = axk::FloppyImportSource::open_directories({{"one", {entry("A.001", segment(true))}},
                                                                   {"copy", {entry("B.001", segment(true))}},
                                                                   {"two", {entry("C.001", segment(false))}}});
    ASSERT_TRUE(opened) << opened.error().message;
    ASSERT_EQ(opened->inspection().objects.size(), 1U);
    const auto &wave = opened->inspection().objects.front();
    EXPECT_TRUE(wave.exclusion_reason.empty());
    EXPECT_EQ(wave.sources.size(), 3U);
    const auto prepared = opened->prepare(std::array{wave.key});
    ASSERT_TRUE(prepared) << prepared.error().message;
    EXPECT_EQ(prepared->nodes.front().raw_payload, smpl_object());
    auto conflicting = segment(true);
    conflicting[0xacU] = std::byte{0x7f};
    const auto failed = axk::FloppyImportSource::open_directories({{"one", {entry("A.001", segment(true))}},
                                                                   {"copy", {entry("B.001", conflicting)}},
                                                                   {"two", {entry("C.001", segment(false))}}});
    ASSERT_TRUE(failed) << failed.error().message;
    EXPECT_FALSE(failed->inspection().can_import);
}

TEST(FloppyRecovery, DoesNotJoinDifferentHeadersAndRejectsOversizedDeclaredPayloads) {
    auto second = segment(false);
    second[0x43U] = std::byte{1};
    const auto opened = axk::FloppyImportSource::open_directories(
        {{"one", {entry("A.001", segment(true))}}, {"two", {entry("B.001", second)}}});
    ASSERT_TRUE(opened) << opened.error().message;
    EXPECT_FALSE(opened->inspection().can_import);
    auto huge = segment(true);
    be32(huge, 0x1cU, 0xffffffffU);
    EXPECT_FALSE(axk::FloppyImportSource::open_directories({{"huge", {entry("A.001", huge)}}}));
}

TEST(FloppyRecovery, KeepsUnreadableRecognizedHeadersVisibleWithoutGuessingTheirType) {
    auto bad = smpl_object();
    bad.resize(0x10U);
    const auto opened = axk::FloppyImportSource::open_directories(
        {{"disk", {entry("BAD.001", bad), entry("GOOD.002", smpl_object())}}});
    ASSERT_TRUE(opened) << opened.error().message;
    EXPECT_TRUE(opened->inspection().can_import);
    EXPECT_TRUE(opened->inspection().requires_acknowledgement);
    ASSERT_EQ(opened->inspection().excluded_files.size(), 1U);
    EXPECT_TRUE(opened->inspection().excluded_files.front().unreadable_object);
    EXPECT_FALSE(opened->inspection().excluded_files.front().reason.empty());
}
