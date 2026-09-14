#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/system_file.hpp"

namespace {
using Duration = axk::SystemRemixDuration;
using Processing = axk::SystemRemixProcessing;

axk::DecodedSystemFile retained(bool native, std::uint8_t revision = 0) {
    const auto size = native ? 0x400U : 0x1000U;
    std::vector<std::byte> bytes(axk::current_record_envelope_size + size, std::byte{0xa5});
    axk::ByteWriter writer{bytes};
    EXPECT_TRUE(writer.write_ascii_field(0, 12, "FSFSDEV3SPLX", std::byte{}));
    EXPECT_TRUE(writer.write_ascii_field(12, 4, "PRF3", std::byte{}));
    EXPECT_TRUE(writer.write_be32(0x14, 4));
    EXPECT_TRUE(writer.write_be32(0x18, 0x36));
    EXPECT_TRUE(writer.write_be32(0x1c, size + 8U));
    EXPECT_TRUE(writer.write_be32(0x30, native ? 0x21520531U : 0xdeadfaceU));
    bytes[0x3e] = static_cast<std::byte>(revision);
    return axk::decode_system_file(
               native ? axk::SystemFileKind::a3000_system : axk::SystemFileKind::a4000_a5000_system2, bytes)
        .value();
}

axk::SystemRegisteredRemixPatch recipe(std::uint8_t slot = 0) {
    return {slot,
            {{Duration::quarter, Processing::random_slice, 255},
             {Duration::quarter, Processing::reverse_random_slice, 130},
             {Duration::eighth, Processing::fixed_slice, 0},
             {Duration::eighth, Processing::silence, 0},
             {Duration::eighth, Processing::lo_fi_random_slice, 5},
             {Duration::eighth, Processing::pitch_random_slice, 6}}};
}

void replace_expected(axk::DecodedSystemFile &file, const axk::SystemRegisteredRemixPatch &patch) {
    const auto slot = static_cast<std::size_t>(patch.slot) * 24U;
    for (std::size_t i = 0; i < 24U; ++i) {
        const bool active = i < patch.steps.size();
        file.system_bulk_bytes[0x60U + slot + i] =
            active ? static_cast<std::byte>(patch.steps[i].duration) : std::byte{};
        file.system_bulk_bytes[0xd8U + slot + i] =
            active ? static_cast<std::byte>(patch.steps[i].random_choice) : std::byte{};
        file.system_bulk_bytes[0x150U + slot + i] =
            active ? static_cast<std::byte>(patch.steps[i].processing) : std::byte{};
    }
}
} // namespace

TEST(SystemRemixWrite, EachSlotAndRevisionChangesOnlyItsThreeLanes) {
    for (const auto model : {axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        for (std::uint8_t revision : {std::uint8_t{0}, std::uint8_t{1}}) {
            const auto file = retained(false, revision);
            for (std::uint8_t slot = 0; slot < 5; ++slot) {
                const std::array patches{recipe(slot)};
                auto expected = file;
                replace_expected(expected, patches[0]);
                const auto updated = axk::patch_system_registered_remix(file, patches, model);
                ASSERT_TRUE(updated) << updated.error().message;
                EXPECT_EQ(*updated, expected);
                EXPECT_EQ(axk::patch_system_registered_remix(*updated, patches, model).value(), expected);
                const auto decoded = axk::decode_system_global(*updated);
                ASSERT_TRUE(decoded);
                ASSERT_TRUE(decoded->registered_remix);
                EXPECT_EQ((*decoded->registered_remix)[slot].random_choices[0], 255);
                EXPECT_EQ((*decoded->registered_remix)[slot].duration_codes[6], 0);
                const std::array reset{axk::SystemRegisteredRemixPatch{slot, {}}};
                replace_expected(expected, reset[0]);
                EXPECT_EQ(axk::patch_system_registered_remix(*updated, reset, model).value(), expected);
            }
        }
    }
}

TEST(SystemRemixWrite, RejectsInvalidBatchesModelsAndMalformedRetainedMetadata) {
    const auto file = retained(false);
    const auto original = file;
    const auto model = axk::ASeriesModel::a5000;
    EXPECT_EQ(axk::patch_system_registered_remix(file, {}, model).value(), file);
    EXPECT_EQ(axk::patch_system_registered_remix(retained(true), {}, axk::ASeriesModel::a3000).value(), retained(true));
    EXPECT_FALSE(axk::patch_system_registered_remix(retained(true), std::array{recipe()}, axk::ASeriesModel::a3000));
    EXPECT_FALSE(axk::patch_system_registered_remix(file, {}, axk::ASeriesModel::a3000));
    EXPECT_FALSE(axk::patch_system_registered_remix(file, {}, static_cast<axk::ASeriesModel>(255)));
    EXPECT_FALSE(axk::patch_system_registered_remix(file, std::array{recipe(), recipe()}, model));
    EXPECT_FALSE(axk::patch_system_registered_remix(file, std::array{recipe(), recipe(5)}, model));
    auto malformed = file;
    malformed.system_bulk_bytes.pop_back();
    EXPECT_FALSE(axk::patch_system_registered_remix(malformed, {}, model));
    EXPECT_EQ(file, original);
}

TEST(SystemRemixWrite, EveryRawDurationAndProcessingValueHasAnExplicitDomain) {
    const auto file = retained(false);
    const auto model = axk::ASeriesModel::a5000;
    for (unsigned raw = 0; raw < 256U; ++raw) {
        auto p = recipe();
        p.steps = std::vector<axk::SystemRemixStep>(raw == 2 ? 4U : 8U);
        for (auto &step : p.steps)
            step.duration = static_cast<Duration>(raw);
        EXPECT_EQ(axk::patch_system_registered_remix(file, std::array{p}, model).has_value(), raw == 1 || raw == 2)
            << raw;
        p = recipe();
        p.steps[0].processing = static_cast<Processing>(raw);
        p.steps[0].random_choice = 0;
        const bool supported = raw == 0 || raw == 1 || raw == 3 || raw == 5 || raw == 6 || raw == 11 || raw == 19;
        EXPECT_EQ(axk::patch_system_registered_remix(file, std::array{p}, model).has_value(), supported) << raw;
    }
    auto short_recipe = recipe();
    short_recipe.steps.pop_back();
    EXPECT_FALSE(axk::patch_system_registered_remix(file, std::array{short_recipe}, model));
    auto long_recipe = recipe();
    long_recipe.steps.push_back({});
    EXPECT_FALSE(axk::patch_system_registered_remix(file, std::array{long_recipe}, model));
    auto too_many = recipe();
    too_many.steps.resize(24);
    EXPECT_FALSE(axk::patch_system_registered_remix(file, std::array{too_many}, model));
    auto unused = recipe();
    unused.steps[2].random_choice = 1;
    EXPECT_FALSE(axk::patch_system_registered_remix(file, std::array{unused}, model));
}

TEST(SystemRemixWrite, CombinedReplacementCanSelectUserOneThroughThreeButNotLoadSensitiveSelections) {
    const auto file = retained(false);
    for (std::uint8_t type = 5; type <= 9; ++type) {
        axk::SystemFilePatch patch;
        patch.registered_remix = {recipe(static_cast<std::uint8_t>(type - 5U))};
        patch.global.remix_type_selection = type;
        patch.global.remix_variation_selection = 7;
        const auto updated = axk::patch_system_file(file, patch, axk::ASeriesModel::a5000);
        ASSERT_EQ(updated.has_value(), type <= 7);
        if (updated) {
            EXPECT_EQ(axk::decode_system_global(*updated)->parameters.remix_type_selection, type);
            auto expected = file;
            replace_expected(expected, patch.registered_remix[0]);
            expected.system_bulk_bytes[0x34] = static_cast<std::byte>((static_cast<unsigned>(type) << 4U) | 7U);
            EXPECT_EQ(*updated, expected);
        }
    }
    axk::SystemGlobalParameters selection;
    selection.remix_type_selection = 5;
    selection.remix_variation_selection = 0;
    EXPECT_FALSE(axk::patch_system_global(file, selection, axk::ASeriesModel::a5000));
}

TEST(SystemRemixWrite, ReplacesAllFiveSlotsInOneBatchAndRejectsAnOversizedBatch) {
    const auto file = retained(false);
    const std::array patches{recipe(4), recipe(1), recipe(3), recipe(0), recipe(2)};
    auto expected = file;
    for (const auto &patch : patches)
        replace_expected(expected, patch);
    const auto updated = axk::patch_system_registered_remix(file, patches, axk::ASeriesModel::a4000);
    ASSERT_TRUE(updated) << updated.error().message;
    EXPECT_EQ(*updated, expected);
    auto excessive = std::vector(patches.begin(), patches.end());
    excessive.push_back(recipe());
    EXPECT_FALSE(axk::patch_system_registered_remix(file, excessive, axk::ASeriesModel::a4000));
}

TEST(SystemRemixWrite, RandomChoicePreservesEveryByteOnlyForRandomSourceOperations) {
    const auto file = retained(false);
    for (const auto processing :
         {Processing::copy, Processing::random_slice, Processing::reverse_random_slice, Processing::fixed_slice,
          Processing::silence, Processing::lo_fi_random_slice, Processing::pitch_random_slice}) {
        const bool random = processing != Processing::copy && processing != Processing::fixed_slice &&
                            processing != Processing::silence;
        for (unsigned raw = 0; raw < 256U; ++raw) {
            auto patch = recipe();
            patch.steps[0].processing = processing;
            patch.steps[0].random_choice = static_cast<std::uint8_t>(raw);
            const auto updated = axk::patch_system_registered_remix(file, std::array{patch}, axk::ASeriesModel::a5000);
            ASSERT_EQ(updated.has_value(), random || raw == 0U) << raw;
            if (updated) {
                auto expected = file;
                replace_expected(expected, patch);
                EXPECT_EQ(*updated, expected);
            }
        }
    }
}

TEST(SystemRemixWrite, SelectionValidatesOnlyTheEffectiveRecipeAndIgnoresTerminatedTails) {
    auto file = retained(false);
    replace_expected(file, recipe());
    for (const auto lane : {0x60U, 0xd8U, 0x150U})
        for (std::size_t i = 7; i < 24; ++i)
            file.system_bulk_bytes[lane + i] = std::byte{0xff};
    axk::SystemGlobalParameters selection;
    selection.remix_type_selection = 5;
    selection.remix_variation_selection = 0;
    auto expected = file;
    expected.system_bulk_bytes[0x34] = std::byte{0x50};
    EXPECT_EQ(axk::patch_system_global(file, selection, axk::ASeriesModel::a5000).value(), expected);

    selection.remix_type_selection.reset();
    selection.remix_variation_selection = 7;
    auto varied = expected;
    varied.system_bulk_bytes[0x34] = std::byte{0x57};
    EXPECT_EQ(axk::patch_system_global(expected, selection, axk::ASeriesModel::a5000).value(), varied);
    selection.remix_type_selection = 6;
    EXPECT_FALSE(axk::patch_system_global(file, selection, axk::ASeriesModel::a5000));

    selection.remix_type_selection = 5;
    auto unterminated = file;
    for (std::size_t i = 0; i < 24; ++i)
        unterminated.system_bulk_bytes[0x60U + i] = std::byte{1};
    EXPECT_FALSE(axk::patch_system_global(unterminated, selection, axk::ASeriesModel::a5000));
    auto empty = file;
    empty.system_bulk_bytes[0x60] = std::byte{};
    EXPECT_TRUE(axk::patch_system_global(empty, selection, axk::ASeriesModel::a5000));
    EXPECT_FALSE(axk::patch_system_global(retained(true), selection, axk::ASeriesModel::a3000));

    axk::SystemGlobalParameters unrelated;
    unrelated.master_fine_tune = 1;
    auto unrelated_expected = unterminated;
    unrelated_expected.system_bulk_bytes[0x10] = std::byte{1};
    EXPECT_EQ(axk::patch_system_global(unterminated, unrelated, axk::ASeriesModel::a5000).value(), unrelated_expected);
}
