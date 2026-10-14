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
    std::filesystem::path root = std::filesystem::temp_directory_path() / "axk-conversion-file-plan-test";
    axk::conversion::FileRequest request;
    void SetUp() override {
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
                   {{"partitionIndex", 0}, {"volumeName", "Samples"}},
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
            if (issue.impact == axk::conversion::Impact::loss || issue.impact == axk::conversion::Impact::uncertainty)
                result.push_back(issue.id);
        return result;
    }
};
} // namespace

TEST_F(ConversionFiles, PreparesWithoutWritesAndPublishesOnceToANewOutput) {
    const auto before = bytes(request.destination);
    auto plan = axk::conversion::FilePlan::prepare(request);
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_EQ(bytes(request.destination), before);
    EXPECT_FALSE(std::filesystem::exists(root / "output.hds"));
    EXPECT_FALSE(plan->apply(root / "output.hds", {}));
    const auto applied = plan->apply(root / "output.hds", acknowledgements(plan->report()));
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
    ASSERT_TRUE(plan->report().executable) << axk::conversion::report_json(plan->report()).dump();
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
    std::vector<std::string> ids;
    for (const auto &issue : document.at("issues"))
        if (issue.at("impact") == "LOSS" || issue.at("impact") == "UNCERTAINTY")
            ids.push_back(issue.at("id"));
    const auto applied = plan->apply((root / "output.hds").string(), ids, context);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_TRUE(nlohmann::json::parse(*applied).at("applied"));
    EXPECT_FALSE(plan->apply((root / "retry.hds").string(), ids, context));
}
#endif
