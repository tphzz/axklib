#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/conversion/tx16w_to_a_series/plan.hpp"
#include "axklib/conversion/tx16w_to_a_series/profile.hpp"
#include "axklib/writer.hpp"

namespace {
std::vector<std::byte> file(std::size_t size) {
    std::vector<std::byte> bytes(size);
    std::size_t offset{};
    for (const auto c : std::string_view{"LM8953"})
        bytes[offset++] = static_cast<std::byte>(c);
    return bytes;
}
axk::filesystems::Entry entry(std::string name, std::vector<std::byte> bytes) {
    return {name, name, false, 0U, std::make_shared<axk::MemoryReader>(std::move(bytes))};
}
axk::native::SourceMember member(std::string id, std::vector<axk::filesystems::Entry> entries) {
    return {std::move(id),
            "Disk",
            std::make_shared<axk::MemoryReader>(std::vector<std::byte>{}),
            {{"fat", "FAT", std::move(entries)}}};
}
std::vector<std::byte> wave() {
    auto bytes = file(35U);
    bytes[22U] = std::byte{0xc9};
    bytes[23U] = std::byte{2};
    bytes[24U] = std::byte{2};
    bytes[32U] = std::byte{0x7f};
    bytes[33U] = std::byte{0xf0};
    bytes[34U] = std::byte{0x80};
    return bytes;
}

void put_name(std::vector<std::byte> &bytes, std::size_t offset, std::string_view name) {
    for (const auto c : name)
        bytes.at(offset++) = static_cast<std::byte>(c);
}

class Tx16wSourceSelection : public ::testing::Test {
  protected:
    void SetUp() override {
        path = std::filesystem::temp_directory_path() /
               (std::string{"axklib-tx16w-subset-"} + ::testing::UnitTest::GetInstance()->current_test_info()->name() +
                ".hds");
        const axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {{"Disk", {{"Samples", {}, {}, {}, {}}}}}};
        ASSERT_TRUE(axk::write_hds_image(manifest, path, true));
        auto target = axk::FileReader::open(path);
        ASSERT_TRUE(target);
        request.destination = {"yamaha.a-series",
                               *target,
                               path,
                               {{"kind", "EXISTING_VOLUME"}, {"partitionIndex", 0}, {"volumeName", "Samples"}}};
        ASSERT_TRUE(registry.add(axk::tx16w::a_series::profile()));
    }
    void TearDown() override {
        request.destination.reader.reset();
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    void open(std::vector<axk::native::SourceMember> members) {
        auto sources = axk::native::SourceSet::create(std::move(members));
        ASSERT_TRUE(sources);
        auto document = axk::tx16w::Document::open(*sources);
        ASSERT_TRUE(document) << document.error().message;
        request.source = *document;
    }
    void open_hierarchy(bool has_performances, bool named_missing_wave = true, bool shared_timbre = false) {
        auto setup = file(1536U), voices = file(8704U), performances = file(5120U);
        put_name(setup, 0xf7U, "PIANO");
        if (named_missing_wave)
            put_name(setup, 0x107U, "ABSENT");
        for (std::size_t slot = 0; slot < 2U; ++slot) {
            const auto base = 16U + 146U * slot;
            put_name(voices, base + 0x86U, slot == 0U ? "Pno Voice" : "Str Voice");
            voices[base] = static_cast<std::byte>(slot);
            voices[base + 1U] = std::byte{36};
            voices[base + 2U] = std::byte{96};
            for (std::size_t region = 1; region < 32U; ++region)
                voices[base + region * 4U + 1U] = std::byte{0xff};
            const auto timbre = 0x1250U + 56U * slot;
            voices[timbre] = static_cast<std::byte>(slot);
            voices[timbre + 1U] = std::byte{76};
            put_name(voices, timbre + 46U, slot == 0U ? "Pno Mid" : "Str Mid");
        }
        const auto orphan = 0x1250U + 56U * 2U;
        voices[orphan] = std::byte{6};
        put_name(voices, orphan + 46U, "Unused");
        if (shared_timbre) {
            constexpr auto source = 16U + 146U;
            constexpr auto target = source + 146U;
            std::copy_n(voices.begin() + source, 146U, voices.begin() + target);
            voices[target + 1U] = std::byte{97};
            voices[target + 2U] = std::byte{120};
        }
        std::vector<axk::filesystems::Entry> entries{entry("SET.S01", std::move(setup)),
                                                     entry("SET.V01", std::move(voices))};
        if (has_performances) {
            for (std::size_t slot = 0U; slot < (shared_timbre ? 4U : 3U); ++slot) {
                const auto base = 16U + 146U * slot;
                constexpr std::string_view names[]{"Piano", "Piano 2", "Strings", "Strings Alt"};
                put_name(performances, base + 0x78U, names[slot]);
                std::fill_n(performances.begin() + static_cast<std::ptrdiff_t>(base + 18U), 16U, std::byte{0xff});
                performances[base + 18U] = slot < 2U ? std::byte{} : static_cast<std::byte>(slot - 1U);
            }
            entries.push_back(entry("SET.U01", std::move(performances)));
        }
        open({member("one", std::move(entries)), member("two", {entry("PIANO.W01", wave())})});
    }
    std::string object_id(std::string_view name) const {
        const auto &objects = request.source->catalog().objects;
        const auto found = std::ranges::find(objects, name, &axk::native::ObjectSummary::name);
        return found == objects.end() ? std::string{} : found->id;
    }
    auto prepare() const { return registry.prepare("yamaha.tx16w-to-a-series", request); }
    axk::conversion::Request request;
    axk::conversion::Registry registry;
    std::filesystem::path path;
};
} // namespace

TEST_F(Tx16wSourceSelection, SelectAllUsesPlayableRootClosureAndKeepsUnreferencedTimbresBrowsable) {
    for (const bool named : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(open_hierarchy(true, named));
        ASSERT_FALSE(object_id("Unused").empty());
        for (const auto fidelity : {axk::conversion::Fidelity::best, axk::conversion::Fidelity::strict}) {
            request.fidelity = fidelity;
            request.options = {{"mode", "HIERARCHY"}};
            const auto all = prepare();
            ASSERT_TRUE(all) << all.error().message;
            const auto &r = (*all)->report();
            EXPECT_FALSE(r.executable);
            EXPECT_TRUE(
                std::ranges::none_of(r.issues, [&](const auto &i) { return i.source_object == object_id("Unused"); }));
            const auto bad = std::ranges::find_if(r.issues, [&](const auto &i) {
                return i.source_object == object_id("Str Mid") && i.impact == axk::conversion::Impact::blocker;
            });
            ASSERT_NE(bad, r.issues.end());
            for (const auto token :
                 {"Str Mid", "Wave slot 2", named ? "ABSENT.W02" : "blank", "Str Voice", "36..96", "Strings"})
                EXPECT_NE(bad->message.find(token), std::string::npos) << bad->message;
            request.options["sourceObjectIds"] = {object_id("Piano"), object_id("Piano 2"), object_id("Strings")};
            const auto explicit_all = prepare();
            ASSERT_TRUE(explicit_all);
            EXPECT_EQ((*explicit_all)->report().issues.size(), r.issues.size());
            EXPECT_EQ((*explicit_all)->report().actions.size(), r.actions.size());
        }
    }
}

TEST_F(Tx16wSourceSelection, SelectedPerformancesShareDependenciesAndExcludeAnUnrelatedMissingWave) {
    ASSERT_NO_FATAL_FAILURE(open_hierarchy(true));
    const auto piano = object_id("Piano"), layered = object_id("Piano 2"), strings = object_id("Strings");
    ASSERT_FALSE(piano.empty());
    ASSERT_FALSE(layered.empty());
    ASSERT_FALSE(strings.empty());
    request.options = {{"mode", "HIERARCHY"}};
    const auto whole = prepare();
    ASSERT_TRUE(whole) << whole.error().message;
    EXPECT_FALSE((*whole)->report().executable);
    request.options["sourceObjectIds"] = {piano, layered};
    const auto subset = prepare();
    ASSERT_TRUE(subset) << subset.error().message;
    const auto &report = (*subset)->report();
    ASSERT_TRUE(report.executable);
    ASSERT_EQ(report.actions.size(), 5U);
    EXPECT_EQ(std::ranges::count(report.source_objects, true, &axk::conversion::SourceObject::selectable), 3);
    for (const auto &id : {piano, layered}) {
        const auto root = std::ranges::find(report.source_objects, id, &axk::conversion::SourceObject::id);
        ASSERT_NE(root, report.source_objects.end());
        EXPECT_TRUE(root->selectable);
        EXPECT_EQ(root->dependencies, (std::vector<std::string>{object_id("Pno Voice")}));
    }
    const auto dependency =
        std::ranges::find(report.source_objects, object_id("Pno Voice"), &axk::conversion::SourceObject::id);
    ASSERT_NE(dependency, report.source_objects.end());
    EXPECT_FALSE(dependency->selectable);
    EXPECT_EQ(dependency->dependencies, (std::vector<std::string>{object_id("Pno Mid")}));
    for (const auto &id : {piano, layered, object_id("Pno Voice"), object_id("Pno Mid"), object_id("PIANO.W01")})
        EXPECT_EQ(std::ranges::count(report.actions, id, &axk::conversion::Action::source_object), 1);
    request.options["sourceObjectIds"] = {strings};
    const auto blocked = prepare();
    ASSERT_TRUE(blocked) << blocked.error().message;
    EXPECT_FALSE((*blocked)->report().executable);
    const auto &issues = (*blocked)->report().issues;
    EXPECT_EQ(std::ranges::count_if(issues,
                                    [&](const auto &issue) {
                                        return issue.source_object == object_id("Str Mid") &&
                                               issue.impact == axk::conversion::Impact::blocker;
                                    }),
              1);
    const auto owned = std::ranges::find(issues, object_id("Str Mid"), &axk::conversion::Issue::source_object);
    ASSERT_NE(owned, issues.end());
    EXPECT_NE(owned->message.find("required by Strings, Disk 1:"), std::string::npos);
    EXPECT_TRUE(std::get<axk::conversion::ExistingImagePatches>((*blocked)->output()).patches.empty());
}

TEST_F(Tx16wSourceSelection, SharedBrokenTimbreNamesOneActualSelectedVoiceOccurrenceAndRegion) {
    ASSERT_NO_FATAL_FAILURE(open_hierarchy(true, false, true));
    for (bool both : {true, false}) {
        request.options = {{"mode", "HIERARCHY"}, {"sourceObjectIds", {object_id("Strings Alt")}}};
        if (both)
            request.options["sourceObjectIds"].push_back(object_id("Strings"));
        const auto prepared = prepare();
        ASSERT_TRUE(prepared);
        const auto &issues = (*prepared)->report().issues;
        const auto bad = [&](const auto &issue) {
            return issue.source_object == object_id("Str Mid") && issue.impact == axk::conversion::Impact::blocker;
        };
        ASSERT_EQ(std::ranges::count_if(issues, bad), 1);
        const auto &message = std::ranges::find_if(issues, bad)->message;
        EXPECT_NE(message.find(both ? "example: required by Strings," : "example: required by Strings Alt,"),
                  std::string::npos);
        EXPECT_NE(message.find(both ? "/ voice 2, native keys 36..96" : "/ voice 3, native keys 97..120"),
                  std::string::npos)
            << message;
        EXPECT_EQ(message.find(both ? "97..120" : "36..96"), std::string::npos);
    }
}

TEST_F(Tx16wSourceSelection, SelectsVoiceRootsOnlyWhenPerformanceFileIsAbsent) {
    ASSERT_NO_FATAL_FAILURE(open_hierarchy(true));
    request.options = {{"mode", "HIERARCHY"}, {"sourceObjectIds", {object_id("Pno Voice")}}};
    EXPECT_FALSE(prepare());
    ASSERT_NO_FATAL_FAILURE(open_hierarchy(false));
    const auto voice = object_id("Pno Voice");
    ASSERT_FALSE(voice.empty());
    request.options["sourceObjectIds"] = {voice};
    const auto subset = prepare();
    ASSERT_TRUE(subset) << subset.error().message;
    const auto &report = (*subset)->report();
    ASSERT_TRUE(report.executable);
    ASSERT_EQ(report.actions.size(), 4U);
    const auto program = std::ranges::find(report.actions, "program", &axk::conversion::Action::kind);
    ASSERT_NE(program, report.actions.end());
    EXPECT_EQ(program->source_object, voice);
}

TEST_F(Tx16wSourceSelection, SelectsWavesByOccurrenceAndRejectsInvalidExplicitSelections) {
    ASSERT_NO_FATAL_FAILURE(
        open({member("one", {entry("TONE.W01", wave())}), member("two", {entry("TONE.W01", wave())})}));
    const auto &objects = request.source->catalog().objects;
    ASSERT_EQ(objects.size(), 2U);
    ASSERT_NE(objects[0].id, objects[1].id);
    request.options = {{"mode", "WAVE_DATA_ONLY"}, {"sourceObjectIds", {objects[1].id}}};
    const auto subset = prepare();
    ASSERT_TRUE(subset) << subset.error().message;
    ASSERT_TRUE((*subset)->report().executable);
    ASSERT_EQ((*subset)->report().actions.size(), 1U);
    EXPECT_EQ((*subset)->report().actions.front().source_object, objects[1].id);
    const auto &selectable = (*subset)->report().source_objects;
    ASSERT_EQ(selectable.size(), 2U);
    EXPECT_TRUE(selectable[0].selectable);
    EXPECT_TRUE(selectable[1].selectable);
    EXPECT_EQ(selectable[0].name, selectable[1].name);
    EXPECT_NE(selectable[0].location, selectable[1].location);
    for (const auto &ids : std::vector<std::vector<std::string>>{{}, {"unknown"}, {objects[0].id, objects[0].id}}) {
        request.options["sourceObjectIds"] = ids;
        EXPECT_FALSE(prepare());
    }
    ASSERT_NO_FATAL_FAILURE(open_hierarchy(true));
    request.options["sourceObjectIds"] = {object_id("Piano")};
    EXPECT_FALSE(prepare());
}

TEST(Tx16wConversionDocument, KeepsOccurrencesAndDefersPcmMaterialization) {
    const auto sources = axk::native::SourceSet::create(
        {member("one", {entry("/TONE.W01", wave())}), member("two", {entry("/Folder/TONE.W01", wave())})});
    ASSERT_TRUE(sources);
    const auto document = axk::tx16w::Document::open(*sources);
    ASSERT_TRUE(document);
    const auto inspection = axk::tx16w::inspect_document(**document, axk::tx16w::ImportMode::wave_data_only);
    ASSERT_TRUE(inspection);
    ASSERT_EQ(inspection->waves.size(), 2U);
    EXPECT_TRUE(inspection->waves.front().pcm.empty());
    EXPECT_EQ(inspection->waves.front().name, "TONE.W01");
    EXPECT_EQ(inspection->waves.back().name, "TONE.W01");
    EXPECT_NE(inspection->wave_object_ids.front(), inspection->wave_object_ids.back());
    const auto plan = axk::tx16w::a_series::plan_import(*inspection, {}, axk::tx16w::ImportMode::wave_data_only);
    ASSERT_TRUE(plan);
    ASSERT_EQ(plan->wave_data.size(), 2U);
    EXPECT_EQ(plan->wave_data.front().name, "TONE");
    EXPECT_NE(plan->wave_data.front().name, plan->wave_data.back().name);
}

TEST(Tx16wConversionDocument, BlocksMissingCompanionsButAllowsWaveOnly) {
    const auto sources =
        axk::native::SourceSet::create({member("one", {entry("SET.S01", file(1536U)), entry("TONE.W01", wave())})});
    ASSERT_TRUE(sources);
    const auto document = axk::tx16w::Document::open(*sources);
    ASSERT_TRUE(document);
    for (const auto mode : {axk::tx16w::ImportMode::hierarchy, axk::tx16w::ImportMode::wave_data_only}) {
        const auto inspection = axk::tx16w::inspect_document(**document, mode);
        ASSERT_TRUE(inspection);
        const auto plan = axk::tx16w::a_series::plan_import(*inspection, {}, mode);
        ASSERT_TRUE(plan);
        EXPECT_EQ(std::ranges::any_of(plan->notices,
                                      [](const auto &notice) {
                                          return notice.disposition ==
                                                 axk::tx16w::a_series::MappingDisposition::blocked;
                                      }),
                  mode == axk::tx16w::ImportMode::hierarchy);
    }
}

TEST(Tx16wConversionDocument, PreparesReviewedPatchesWithoutChangingTheDestination) {
    const auto path = std::filesystem::temp_directory_path() / "axklib-native-conversion-destination.hds";
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } cleanup{path};
    const axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {{"Disk", {{"Samples", {}, {}, {}, {}}}}}};
    ASSERT_TRUE(axk::write_hds_image(manifest, path, true));
    const auto target = axk::FileReader::open(path);
    ASSERT_TRUE(target);
    const auto sources = axk::native::SourceSet::create({member("one", {entry("/Folder/TONE.W01", wave())})});
    ASSERT_TRUE(sources);
    const auto document = axk::tx16w::Document::open(*sources);
    ASSERT_TRUE(document);
    axk::conversion::Registry registry;
    ASSERT_TRUE(registry.add(axk::tx16w::a_series::profile()));
    axk::conversion::Request request;
    request.source = *document;
    request.destination = {"yamaha.a-series",
                           *target,
                           path,
                           {{"kind", "EXISTING_VOLUME"}, {"partitionIndex", 0}, {"volumeName", "Samples"}}};
    request.options = {{"mode", "WAVE_DATA_ONLY"}};
    auto plan = registry.prepare("yamaha.tx16w-to-a-series", request);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE((*plan)->report().executable);
    ASSERT_EQ((*plan)->report().actions.size(), 1U);
    EXPECT_EQ((*plan)->report().actions.front().name, "TONE");
    EXPECT_EQ((*plan)->report().actions.front().source_object, (*document)->native_objects().front().id);
    const auto &patches = std::get<axk::conversion::ExistingImagePatches>((*plan)->output()).patches;
    EXPECT_FALSE(patches.empty());
    EXPECT_TRUE((*plan)->acknowledge({}));
    for (const auto &patch : patches) {
        std::vector<std::byte> actual(patch.original.size());
        ASSERT_TRUE((*target)->read_exact_at(patch.offset, actual));
        EXPECT_EQ(actual, patch.original);
    }
    request.fidelity = axk::conversion::Fidelity::strict;
    const auto strict = registry.prepare("yamaha.tx16w-to-a-series", request);
    ASSERT_TRUE(strict) << strict.error().message;
    EXPECT_FALSE((*strict)->report().executable);
    EXPECT_TRUE(std::get<axk::conversion::ExistingImagePatches>((*strict)->output()).patches.empty());
    request.options["extra"] = true;
    EXPECT_FALSE(registry.prepare("yamaha.tx16w-to-a-series", request));
}
