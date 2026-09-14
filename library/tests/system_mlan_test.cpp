#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/system_file.hpp"

namespace {

axk::DecodedSystemFile retained_mlan(bool native, std::uint8_t revision, unsigned value) {
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
    if (!native) {
        bytes[0x694] = static_cast<std::byte>(value);
        bytes[0x695] = static_cast<std::byte>(value);
    }
    return axk::decode_system_file(
               native ? axk::SystemFileKind::a3000_system : axk::SystemFileKind::a4000_a5000_system2, bytes)
        .value();
}

} // namespace

TEST(SystemMlan, DecodesEveryStoredValueWithoutNormalizationOrModelInference) {
    for (unsigned value = 0; value < 256U; ++value) {
        const auto file = retained_mlan(false, 1, value);
        const auto before = axk::encode_system_file(file).value();
        const auto result = axk::decode_system_mlan(file);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->parameters.midi_input.has_value(), value <= 2U);
        EXPECT_EQ(result->parameters.audio_input.has_value(), value <= 1U);
        if (result->parameters.midi_input)
            EXPECT_EQ(static_cast<unsigned>(*result->parameters.midi_input), value);
        if (result->parameters.audio_input)
            EXPECT_EQ(static_cast<unsigned>(*result->parameters.audio_input), value);
        for (std::size_t i = 0; i < 16U; ++i)
            EXPECT_EQ(result->raw_bytes[i], before[0x694U + i]);
        EXPECT_EQ(axk::encode_system_file(file).value(), before);
    }
}

TEST(SystemMlan, RejectsUnavailableLayoutsButEmptyPatchesPreserveEverySupportedModelAndRevision) {
    for (const auto model : std::array{axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        const bool native = model == axk::ASeriesModel::a3000;
        for (std::uint8_t revision = 0; revision <= (native ? 0U : 1U); ++revision) {
            const auto file = retained_mlan(native, revision, 255);
            EXPECT_EQ(axk::decode_system_mlan(file).has_value(), !native && revision == 1U);
            EXPECT_EQ(axk::patch_system_mlan(file, {}, model).value(), file);
            EXPECT_EQ(axk::patch_system_file(file, {}, model).value(), file);
            axk::SystemMlanParameters patch;
            patch.audio_input = axk::SystemMlanAudioInput::ad_in;
            EXPECT_EQ(axk::patch_system_mlan(file, patch, model).has_value(), !native && revision == 1U);
        }
    }
}

TEST(SystemMlan, EveryRequestedByteIsCheckedAgainstModelAndRevisionAndChangesOnlyItsOwnByte) {
    for (const auto model : std::array{axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        const bool native = model == axk::ASeriesModel::a3000;
        for (std::uint8_t revision = 0; revision <= (native ? 0U : 1U); ++revision) {
            const auto file = retained_mlan(native, revision, 255);
            const auto before = axk::encode_system_file(file).value();
            const auto exercise = [&](auto select, unsigned maximum, std::size_t offset) {
                for (unsigned value = 0; value < 256U; ++value) {
                    SCOPED_TRACE(static_cast<unsigned>(model));
                    SCOPED_TRACE(revision);
                    SCOPED_TRACE(offset);
                    SCOPED_TRACE(value);
                    axk::SystemMlanParameters patch;
                    using Choice = typename std::remove_cvref_t<decltype(select(patch))>::value_type;
                    select(patch) = static_cast<Choice>(value);
                    const auto result = axk::patch_system_mlan(file, patch, model);
                    ASSERT_EQ(result.has_value(), !native && revision == 1U && value <= maximum);
                    if (result) {
                        auto expected = before;
                        expected[offset] = static_cast<std::byte>(value);
                        EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
                        auto decoded = axk::decode_system_mlan(*result).value();
                        EXPECT_EQ(select(decoded.parameters), select(patch));
                        EXPECT_EQ(axk::patch_system_mlan(*result, patch, model).value(), *result);
                    } else {
                        EXPECT_EQ(result.error().code, axk::ErrorCode::invalid_argument);
                        axk::SystemFilePatch mixed;
                        mixed.global.master_fine_tune = 7;
                        mixed.panel.end_type = axk::SystemEndType::beat;
                        mixed.mlan = patch;
                        EXPECT_FALSE(axk::patch_system_file(file, mixed, model));
                    }
                    EXPECT_EQ(axk::encode_system_file(file).value(), before);
                }
            };
            exercise([](auto &p) -> auto & { return p.midi_input; }, model == axk::ASeriesModel::a5000 ? 2U : 1U,
                     0x694U);
            exercise([](auto &p) -> auto & { return p.audio_input; }, 1U, 0x695U);
        }
    }
}

TEST(SystemMlan, CombinedEditsPreserveEveryInitializationByteAndDoNotRequireHardwareReadyState) {
    for (unsigned marker = 0; marker < 256U; ++marker) {
        auto file = retained_mlan(false, 1, 255);
        file.system_bulk_bytes[0x652] = static_cast<std::byte>(marker);
        file.system_bulk_bytes[0x653] = static_cast<std::byte>(255U - marker);
        const auto before = axk::encode_system_file(file).value();
        axk::SystemFilePatch patch;
        patch.global.master_fine_tune = 7;
        patch.panel.end_type = axk::SystemEndType::beat;
        patch.mlan.midi_input = axk::SystemMlanMidiInput::mlan_b;
        patch.mlan.audio_input = axk::SystemMlanAudioInput::mlan;
        const auto result = axk::patch_system_file(file, patch, axk::ASeriesModel::a5000);
        ASSERT_TRUE(result) << result.error().message;
        auto expected = before;
        expected[0x60] = std::byte{7};
        expected[0x33f] = std::byte{3};
        expected[0x694] = std::byte{2};
        expected[0x695] = std::byte{1};
        EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
        EXPECT_EQ(axk::encode_system_file(file).value(), before);
    }
}

TEST(SystemMlan, RejectsMismatchedModelsMalformedLayoutsAndInconsistentRetainedMetadata) {
    const auto file = retained_mlan(false, 1, 0);
    axk::SystemMlanParameters patch;
    patch.midi_input = axk::SystemMlanMidiInput::midi;
    for (const auto model : std::array{axk::ASeriesModel::a3000, static_cast<axk::ASeriesModel>(255)}) {
        EXPECT_FALSE(axk::patch_system_mlan(file, patch, model));
        EXPECT_FALSE(axk::patch_system_mlan(file, {}, model));
    }
    EXPECT_FALSE(axk::patch_system_mlan(retained_mlan(true, 0, 0), {}, axk::ASeriesModel::a5000));
    for (unsigned defect = 0; defect < 5U; ++defect) {
        auto malformed = file;
        if (defect == 0U)
            malformed.system_bulk_bytes.pop_back();
        else if (defect == 1U)
            malformed.storage_revision = 2;
        else if (defect == 2U)
            malformed.system_header_bytes[0x0e] = std::byte{};
        else if (defect == 3U)
            malformed.kind = static_cast<axk::SystemFileKind>(255);
        else
            malformed.system_header_bytes.clear();
        if (defect <= 1U || defect == 3U)
            EXPECT_FALSE(axk::decode_system_mlan(malformed));
        EXPECT_FALSE(axk::patch_system_mlan(malformed, patch, axk::ASeriesModel::a5000));
        EXPECT_FALSE(axk::patch_system_mlan(malformed, {}, axk::ASeriesModel::a5000));
    }
}
