#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
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
} // namespace

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
    request.destination = {"yamaha.a-series", *target, path, {{"partitionIndex", 0}, {"volumeName", "Samples"}}};
    request.options = {{"mode", "WAVE_DATA_ONLY"}};
    auto plan = registry.prepare("yamaha.tx16w-to-a-series", request);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_TRUE((*plan)->report().executable);
    ASSERT_EQ((*plan)->report().actions.size(), 1U);
    EXPECT_EQ((*plan)->report().actions.front().name, "TONE");
    EXPECT_EQ((*plan)->report().actions.front().source_object, (*document)->native_objects().front().id);
    EXPECT_FALSE((*plan)->patches().empty());
    EXPECT_FALSE((*plan)->acknowledge({}));
    for (const auto &patch : (*plan)->patches()) {
        std::vector<std::byte> actual(patch.original.size());
        ASSERT_TRUE((*target)->read_exact_at(patch.offset, actual));
        EXPECT_EQ(actual, patch.original);
    }
    request.fidelity = axk::conversion::Fidelity::strict;
    const auto strict = registry.prepare("yamaha.tx16w-to-a-series", request);
    ASSERT_TRUE(strict) << strict.error().message;
    EXPECT_FALSE((*strict)->report().executable);
    EXPECT_TRUE((*strict)->patches().empty());
    request.options["extra"] = true;
    EXPECT_FALSE(registry.prepare("yamaha.tx16w-to-a-series", request));
}
