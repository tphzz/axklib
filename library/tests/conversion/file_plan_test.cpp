#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/audio.hpp"
#include "axklib/conversion/file_plan.hpp"
#include "axklib/conversion/tx16w_to_a_series/profile.hpp"
#include "axklib/media.hpp"
#include "axklib/relationship.hpp"
#include "axklib/writer.hpp"
#ifdef AXK_TEST_SHARED_SDK
#include "axklib/sdk.hpp"
#include "axklib/sdk/conversion.hpp"
#endif

namespace {
class ConversionFiles : public testing::Test {
  protected:
    std::filesystem::path root;
    axk::conversion::FileRequest request;
    void SetUp() override {
        root = std::filesystem::temp_directory_path() /
               (std::string{"axk-conversion-files-"} + testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "source");
        std::vector<char> bytes(35U);
        const std::string magic = "LM8953";
        std::copy(magic.begin(), magic.end(), bytes.begin());
        bytes[22U] = static_cast<char>(0xc9);
        bytes[23U] = 2;
        bytes[24U] = 2;
        bytes[32U] = 0x7f;
        bytes[33U] = static_cast<char>(0xf0);
        bytes[34U] = static_cast<char>(0x80);
        {
            std::ofstream wave{root / "source" / "TONE.W01", std::ios::binary};
            wave.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        const axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {{"Disk", {{"Samples", {}, {}, {}, {}}}}}};
        ASSERT_TRUE(axk::write_hds_image(manifest, root / "target.hds", false));
        request = {"yamaha.tx16w-to-a-series",
                   {root / "source"},
                   root / "target.hds",
                   {{"kind", "EXISTING_VOLUME"}, {"partitionIndex", 0}, {"volumeName", "Samples"}},
                   {{"mode", "WAVE_DATA_ONLY"}},
                   {},
                   {}};
    }
    void TearDown() override { std::filesystem::remove_all(root); }
    static std::vector<char> bytes(const std::filesystem::path &path) {
        std::ifstream file{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    }
    static std::vector<std::string> acknowledgements(const axk::conversion::Report &report) {
        std::vector<std::string> result;
        for (const auto &issue : report.issues)
            if (!issue.decision.empty())
                result.push_back(issue.id);
        return result;
    }
};
} // namespace

TEST_F(ConversionFiles, CreatesVolumeInSecondPartitionOnlyWhenApplyingAndNeverMergesANameCollision) {
    const axk::HdsBuildManifest manifest{
        "1.0", 8U * 1024U * 1024U, {{"First", {{"Imported", {}, {}, {}, {}}}}, {"Second", {}}}};
    ASSERT_TRUE(axk::write_hds_image(manifest, request.destination, true));
    const auto before = bytes(request.destination);
    const auto plan = [&] { return axk::conversion::FilePlan::prepare(request); };
    request.selection = {{"kind", "CREATE_VOLUME"}, {"partitionIndex", 0}, {"volumeName", "Imported"}};
    EXPECT_FALSE(plan());
    request.selection["partitionIndex"] = 1;
    for (const auto name : {"", " padding", "PRF3", "12345678901234567"}) {
        request.selection["volumeName"] = name;
        EXPECT_FALSE(plan()) << name;
    }
    request.selection["volumeName"] = "Imported";
    request.selection.erase("kind");
    EXPECT_FALSE(plan());
    request.selection["kind"] = "CREATE_VOLUME";
    auto prepared = plan();
    ASSERT_TRUE(prepared) << prepared.error().message;
    ASSERT_TRUE(prepared->report().executable);
    EXPECT_EQ(bytes(request.destination), before);
    const auto output = root / "created.hds";
    const auto written = prepared->apply(output, acknowledgements(prepared->report()));
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(bytes(request.destination), before);
    const auto opened = axk::open_image(output);
    ASSERT_TRUE(opened);
    const auto catalog = axk::build_object_catalog(*opened);
    ASSERT_TRUE(catalog);
    ASSERT_EQ(catalog->objects.size(), 1U);
    ASSERT_TRUE(catalog->objects.front().placement);
    EXPECT_EQ(catalog->objects.front().placement->volume_name, "Imported");
    EXPECT_EQ(catalog->objects.front().partition.value, 1U);
}

TEST_F(ConversionFiles, OmittingReverseProfileLeavesNativeWaveReadingAndForwardConversionIntact) {
    axk::conversion::Registry forward_only;
    ASSERT_TRUE(forward_only.add(axk::tx16w::a_series::profile()));
    const auto before = bytes(root / "source" / "TONE.W01");
    auto plan = axk::conversion::FilePlan::prepare(request, {}, nullptr, forward_only);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE(plan->report().executable);
    ASSERT_TRUE(plan->apply(root / "forward.hds", acknowledgements(plan->report())));
    const auto reopened = axk::open_image(root / "forward.hds");
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto catalog = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(catalog);
    ASSERT_EQ(catalog->objects.size(), 1U);
    EXPECT_EQ(catalog->objects.front().object.header.type, axk::ObjectType::smpl);
    EXPECT_EQ(bytes(root / "source" / "TONE.W01"), before);
    request.profile_id = "yamaha.a-series-to-tx16w.wave-disk";
    request.sources = {root / "forward.hds"};
    request.destination.clear();
    request.selection = nullptr;
    const auto rejected = axk::conversion::FilePlan::prepare(request, {}, nullptr, forward_only);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().message, "Unknown conversion profile");
}

TEST_F(ConversionFiles, PublishesOnlyTheRetainedNewImageWithoutATemplate) {
    using namespace axk::conversion;
    const auto expected = bytes(root / "target.hds");
    const auto source = root / "source" / "TONE.W01";
    const auto before = bytes(source);
    std::filesystem::remove(root / "target.hds");
    unsigned calls{};
    Registry registry;
    Profile producer{"test.new-image",
                     "Test image",
                     "yamaha.tx16w",
                     "test.image",
                     {},
                     [&](const Request &input, const axk::CancellationToken &,
                         axk::ProgressSink *) -> axk::Result<std::shared_ptr<const PreparedPlan>> {
                         ++calls;
                         Report report{"test.new-image", input.fidelity, true, {}, {}, {}};
                         report.issues.push_back({"L", "fixture.loss", Impact::loss, {}, {}, {}, {}, "Fixture loss"});
                         const auto data = std::as_bytes(std::span{expected});
                         return std::make_shared<const PreparedPlan>(
                             std::move(report), NewImage{{data.begin(), data.end()}}, data.size() + 4096U);
                     },
                     OutputKind::new_image};
    ASSERT_TRUE(registry.add(std::move(producer)));
    request.profile_id = "test.new-image";
    request.destination.clear();
    request.selection = nullptr;
    const auto output = root / "published.img";
    auto prepare = [&] { return FilePlan::prepare(request, {}, nullptr, registry); };
    auto plan = prepare();
    ASSERT_TRUE(plan) << plan.error().message;
    const auto review = plan->report_json();
    EXPECT_FALSE(review.contains("capacity"));
    EXPECT_EQ(review.at("newImage").at("sizeBytes"), expected.size());
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_FALSE(plan->apply(output, std::vector<std::string>{"L"}));
    EXPECT_FALSE(std::filesystem::exists(output));
    const auto ids = acknowledgements(plan->report());
    EXPECT_TRUE(ids.empty());
    ASSERT_TRUE(plan->apply(output, ids));
    EXPECT_EQ(calls, 1U);
    EXPECT_EQ(bytes(output), expected);
    EXPECT_EQ(bytes(source), before);
    EXPECT_FALSE(plan->apply(root / "again.img", ids));

    auto collision = prepare();
    ASSERT_TRUE(collision);
    EXPECT_FALSE(collision->apply(source, ids));
    EXPECT_EQ(bytes(source), before);
    auto stale = prepare();
    ASSERT_TRUE(stale);
    {
        std::ofstream changed{source, std::ios::app | std::ios::binary};
        changed.put('x');
    }
    EXPECT_FALSE(stale->apply(root / "stale.img", ids));
    EXPECT_FALSE(std::filesystem::exists(root / "stale.img"));
    request.destination = output;
    EXPECT_FALSE(prepare());
    EXPECT_EQ(calls, 3U);
}

TEST_F(ConversionFiles, PreparesWithoutWritesAndPublishesOnceToANewOutput) {
    const auto before = bytes(request.destination);
    auto plan = axk::conversion::FilePlan::prepare(request);
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_EQ(bytes(request.destination), before);
    EXPECT_FALSE(std::filesystem::exists(root / "output.hds"));
    ASSERT_TRUE(std::ranges::any_of(plan->report().issues,
                                    [](const auto &issue) { return issue.impact == axk::conversion::Impact::loss; }));
    const auto applied = plan->apply(root / "output.hds", {});
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(bytes(request.destination), before);
    EXPECT_NE(bytes(root / "output.hds"), before);
    EXPECT_FALSE(plan->apply(root / "second.hds", acknowledgements(plan->report())));
    EXPECT_FALSE(std::filesystem::exists(root / "second.hds"));
}

TEST_F(ConversionFiles, ConvertsNativeHierarchyWithOccurrenceIdsAndReadableAudio) {
    const auto native_file = [](std::size_t size) {
        std::vector<char> data(size);
        const std::string magic = "LM8953";
        std::copy(magic.begin(), magic.end(), data.begin());
        return data;
    };
    auto setup = native_file(1536U), voices = native_file(8704U), performances = native_file(5120U);
    const auto name = [](auto &data, std::size_t offset, const std::string &value) {
        std::copy(value.begin(), value.end(), data.begin() + static_cast<std::ptrdiff_t>(offset));
    };
    name(setup, 0xf0U + 7U, "TONE");
    name(voices, 0x10U + 0x86U, "Voice");
    voices[0x11U] = 36;
    voices[0x12U] = 96;
    for (std::size_t region = 1U; region < 32U; ++region) {
        voices[0x11U + region * 4U] = static_cast<char>(0xff);
        voices[0x12U + region * 4U] = static_cast<char>(0xff);
    }
    voices[0x1251U] = 76;
    name(voices, 0x1250U + 46U, "Timbre");
    name(performances, 0x10U + 0x78U, "Performance");
    for (const auto &[filename, data] : std::vector<std::pair<std::string, std::vector<char>>>{
             {"SET.S01", setup}, {"SET.V01", voices}, {"SET.U01", performances}}) {
        std::ofstream file{root / "source" / filename, std::ios::binary};
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    request.options["mode"] = "HIERARCHY";
    auto plan = axk::conversion::FilePlan::prepare(request);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE(plan->report().executable) << plan->report_json().dump();
    ASSERT_EQ(plan->report().actions.size(), 4U);
    for (const auto &action : plan->report().actions) {
        EXPECT_FALSE(action.source_object.empty());
        EXPECT_FALSE(action.details.empty());
    }
    ASSERT_TRUE(plan->apply(root / "output.hds", acknowledgements(plan->report())));
    const auto image = axk::open_image(root / "output.hds");
    ASSERT_TRUE(image);
    const auto catalog = axk::build_object_catalog(*image);
    ASSERT_TRUE(catalog);
    EXPECT_EQ(catalog->objects.size(), 4U);
    const auto wave = std::ranges::find_if(
        catalog->objects, [](const auto &item) { return item.object.header.type == axk::ObjectType::smpl; });
    ASSERT_NE(wave, catalog->objects.end());
    const auto audio = axk::decode_waveform(*image, *wave);
    ASSERT_TRUE(audio) << audio.error().message;
    EXPECT_EQ(audio->format.sample_rate, 48000U);
    EXPECT_GT(audio->frame_count, 0U);
    EXPECT_FALSE(audio->pcm.empty());

    std::filesystem::remove(root / "source" / "SET.U01");
    auto voice_only = axk::conversion::FilePlan::prepare(request);
    ASSERT_TRUE(voice_only) << voice_only.error().message;
    ASSERT_TRUE(voice_only->report().executable);
    const auto &issues = voice_only->report().issues;
    const auto choice = std::ranges::find(issues, "tx16w.missing_performance", &axk::conversion::Issue::code);
    ASSERT_NE(choice, issues.end());
    EXPECT_FALSE(choice->decision.empty());
    const auto voice_out = root / "voice.hds";
    EXPECT_FALSE(voice_only->apply(voice_out, {}));
    EXPECT_FALSE(std::filesystem::exists(voice_out));
    const auto applied = voice_only->apply(voice_out, std::vector<std::string>{choice->id});
    ASSERT_TRUE(applied) << applied.error().message;
}

TEST_F(ConversionFiles, NeverOverwritesAnExistingDestinationAndConsumesFailedAttempts) {
    const auto before = bytes(request.destination);
    auto plan = axk::conversion::FilePlan::prepare(request);
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_FALSE(plan->apply(request.destination, acknowledgements(plan->report())));
    EXPECT_EQ(bytes(request.destination), before);
    EXPECT_FALSE(plan->apply(root / "retry.hds", acknowledgements(plan->report())));
    EXPECT_FALSE(std::filesystem::exists(root / "retry.hds"));
}

TEST_F(ConversionFiles, RejectsSourceChangesAndFolderMembershipChanges) {
    for (const bool add_member : {false, true}) {
        auto plan = axk::conversion::FilePlan::prepare(request);
        ASSERT_TRUE(plan) << plan.error().message;
        const auto changed = root / "source" / (add_member ? "NEW.W02" : "TONE.W01");
        {
            std::ofstream file{changed, std::ios::app | std::ios::binary};
            file.put('X');
        }
        EXPECT_FALSE(plan->apply(root / "output.hds", acknowledgements(plan->report())));
        EXPECT_FALSE(std::filesystem::exists(root / "output.hds"));
        if (!add_member)
            std::filesystem::resize_file(changed, 35U);
    }
}

TEST_F(ConversionFiles, RejectsDestinationChangesAndCancelledExecution) {
    auto plan = axk::conversion::FilePlan::prepare(request);
    ASSERT_TRUE(plan) << plan.error().message;
    {
        std::fstream target{request.destination, std::ios::binary | std::ios::in | std::ios::out};
        target.seekp(-1, std::ios::end);
        target.put('X');
    }
    EXPECT_FALSE(plan->apply(root / "output.hds", acknowledgements(plan->report())));
    EXPECT_FALSE(std::filesystem::exists(root / "output.hds"));
    auto another = axk::conversion::FilePlan::prepare(request);
    ASSERT_TRUE(another) << another.error().message;
    axk::CancellationSource cancel;
    cancel.cancel();
    EXPECT_FALSE(another->apply(root / "output.hds", acknowledgements(another->report()), cancel.token()));
    EXPECT_FALSE(another->apply(root / "retry.hds", acknowledgements(another->report())));
}

#ifdef AXK_TEST_SHARED_SDK
TEST_F(ConversionFiles, InstalledFacadeRetainsReportAndExecutesTheReviewedPlan) {
    axk::operation_context context;
    axk::conversion_request input{request.profile_id,       {(root / "source").string()}, request.destination.string(),
                                  request.selection.dump(), request.options.dump(),       false};
    auto plan = axk::conversion_plan::prepare(input, context);
    ASSERT_TRUE(plan) << plan.error().message;
    auto report = plan->report_json();
    ASSERT_TRUE(report);
    const auto document = nlohmann::json::parse(*report);
    EXPECT_TRUE(document.at("executable"));
    ASSERT_EQ(document.at("sourceCount"), 1U);
    ASSERT_EQ(document.at("sourceObjects").size(), 1U);
    const auto &wave = document.at("sourceObjects").front();
    EXPECT_EQ(wave.at("kind"), "wave");
    EXPECT_TRUE(wave.at("selectable"));
    EXPECT_FALSE(wave.at("id").get<std::string>().empty());
    EXPECT_FALSE(wave.at("location").get<std::string>().empty());
    EXPECT_EQ(wave.at("dependencies"), nlohmann::json::array());
    EXPECT_TRUE(
        std::ranges::none_of(document.at("issues"), [](const auto &issue) { return issue.contains("decision"); }));
    const auto applied = plan->apply((root / "output.hds").string(), {}, context);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_TRUE(nlohmann::json::parse(*applied).at("applied"));
    EXPECT_FALSE(plan->apply((root / "retry.hds").string(), {}, context));
}
#endif
