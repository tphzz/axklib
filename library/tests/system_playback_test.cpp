#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/system_file.hpp"

namespace {

axk::DecodedSystemFile retained_playback(bool native, std::uint8_t revision, unsigned value = 0xa5U) {
    const auto size = native ? 0x400U : 0x1000U;
    std::vector<std::byte> bytes(axk::current_record_envelope_size + size);
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::byte>((i * 17U + 0x53U) & 0xffU);
    axk::ByteWriter writer{bytes};
    EXPECT_TRUE(writer.write_ascii_field(0, 12, "FSFSDEV3SPLX", std::byte{}));
    EXPECT_TRUE(writer.write_ascii_field(12, 4, "PRF3", std::byte{}));
    EXPECT_TRUE(writer.write_be32(0x14, 4));
    EXPECT_TRUE(writer.write_be32(0x18, 0x36));
    EXPECT_TRUE(writer.write_be32(0x1c, size + 8U));
    EXPECT_TRUE(writer.write_be32(0x30, native ? 0x21520531U : 0xdeadfaceU));
    bytes[0x3e] = static_cast<std::byte>(revision);
    if (!native) {
        bytes[0x67c] = static_cast<std::byte>(value);
        bytes[0x68c] = static_cast<std::byte>(value);
    }
    return axk::decode_system_file(
               native ? axk::SystemFileKind::a3000_system : axk::SystemFileKind::a4000_a5000_system2, bytes)
        .value();
}

constexpr auto current_models = std::array{axk::ASeriesModel::a4000, axk::ASeriesModel::a5000};
constexpr std::array<std::uint8_t, 2> revisions{0, 1};

} // namespace

TEST(SystemPlayback, DecodesEveryStoredByteWithoutClampingAndRetainsBothRegions) {
    for (auto revision : revisions) {
        for (unsigned value = 0; value < 256U; ++value) {
            const auto file = retained_playback(false, revision, value);
            const auto before = axk::encode_system_file(file).value();
            const auto decoded = axk::decode_system_playback(file);
            ASSERT_TRUE(decoded);
            EXPECT_EQ(decoded->parameters.sequence_midi_port.has_value(), value <= 1U);
            EXPECT_EQ(decoded->parameters.digital_output_bits.has_value(), value <= 1U);
            if (value <= 1U) {
                EXPECT_EQ(decoded->parameters.sequence_midi_port, value == 0U ? axk::MidiPort::b : axk::MidiPort::a);
                EXPECT_EQ(decoded->parameters.digital_output_bits,
                          value == 0U ? axk::SystemDigitalOutputBits::bits20 : axk::SystemDigitalOutputBits::bits24);
            }
            EXPECT_EQ((std::vector<std::byte>{decoded->sequence_raw_bytes.begin(), decoded->sequence_raw_bytes.end()}),
                      (std::vector<std::byte>{before.begin() + 0x67c, before.begin() + 0x68c}));
            EXPECT_EQ((std::vector<std::byte>{decoded->digital_output_raw_bytes.begin(),
                                              decoded->digital_output_raw_bytes.end()}),
                      (std::vector<std::byte>{before.begin() + 0x68c, before.begin() + 0x694}));
            EXPECT_EQ(axk::encode_system_file(file).value(), before);
        }
    }
}

TEST(SystemPlayback, ChangesOnlyRequestedBytesAcrossBothRevisionsAndModels) {
    for (auto model : current_models) {
        for (auto revision : revisions) {
            const auto file = retained_playback(false, revision);
            const auto before = axk::encode_system_file(file).value();
            EXPECT_EQ(axk::patch_system_playback(file, {}, model).value(), file);
            for (unsigned field = 0; field < 2U; ++field) {
                for (unsigned value = 0; value < 256U; ++value) {
                    axk::SystemPlaybackParameters patch;
                    if (field == 0U)
                        patch.sequence_midi_port = static_cast<axk::MidiPort>(value);
                    else
                        patch.digital_output_bits = static_cast<axk::SystemDigitalOutputBits>(value);
                    const bool valid = field == 0U ? value == 0U || (value == 1U && model == axk::ASeriesModel::a5000)
                                                   : value == 20U || value == 24U;
                    const auto result = axk::patch_system_playback(file, patch, model);
                    ASSERT_EQ(result.has_value(), valid);
                    if (result) {
                        auto expected = before;
                        expected[field == 0U ? 0x67cU : 0x68cU] = static_cast<std::byte>(field == 0U    ? 1U - value
                                                                                         : value == 24U ? 1U
                                                                                                        : 0U);
                        EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
                        EXPECT_EQ(axk::decode_system_playback(*result)->parameters, patch);
                        EXPECT_EQ(axk::patch_system_playback(*result, patch, model).value(), *result);
                    }
                    EXPECT_EQ(axk::encode_system_file(file).value(), before);
                }
            }
        }
    }
}

TEST(SystemPlayback, ComposesBothSettingsAndRejectsAnInvalidGroupWithoutChangingTheInput) {
    for (auto revision : revisions) {
        const auto file = retained_playback(false, revision);
        const auto before = axk::encode_system_file(file).value();
        axk::SystemFilePatch patch;
        patch.playback.sequence_midi_port = axk::MidiPort::b;
        patch.playback.digital_output_bits = axk::SystemDigitalOutputBits::bits24;
        patch.disk.top_partition = 10;
        auto expected = before;
        expected[0x67c] = std::byte{};
        expected[0x68c] = std::byte{1};
        expected[0x22c] = std::byte{9};
        const auto result = axk::patch_system_file(file, patch, axk::ASeriesModel::a5000);
        ASSERT_TRUE(result) << result.error().message;
        EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
        EXPECT_EQ(axk::decode_system_playback(*result)->parameters, patch.playback);
        patch.playback.digital_output_bits = static_cast<axk::SystemDigitalOutputBits>(16);
        EXPECT_FALSE(axk::patch_system_file(file, patch, axk::ASeriesModel::a5000));
        EXPECT_EQ(axk::encode_system_file(file).value(), before);
    }
}

TEST(SystemPlayback, NativeAllowsOnlyAnEmptyPatchAndModelMismatchIsAlwaysRejected) {
    const auto native = retained_playback(true, 0);
    const auto current = retained_playback(false, 0);
    EXPECT_FALSE(axk::decode_system_playback(native));
    EXPECT_EQ(axk::patch_system_playback(native, {}, axk::ASeriesModel::a3000).value(), native);
    for (bool sequence : {false, true}) {
        axk::SystemPlaybackParameters patch;
        if (sequence)
            patch.sequence_midi_port = axk::MidiPort::a;
        else
            patch.digital_output_bits = axk::SystemDigitalOutputBits::bits20;
        EXPECT_FALSE(axk::patch_system_playback(native, patch, axk::ASeriesModel::a3000));
    }
    for (auto model : current_models)
        EXPECT_FALSE(axk::patch_system_playback(native, {}, model));
    for (auto model : {axk::ASeriesModel::a3000, static_cast<axk::ASeriesModel>(255)})
        EXPECT_FALSE(axk::patch_system_playback(current, {}, model));
    EXPECT_FALSE(axk::patch_system_playback(native, {}, static_cast<axk::ASeriesModel>(255)));
}

TEST(SystemPlayback, DigitalEditsDoNotNormalizeRetainedPortBOnA4000) {
    for (auto revision : revisions) {
        const auto file = retained_playback(false, revision, 0);
        axk::SystemPlaybackParameters patch;
        patch.digital_output_bits = axk::SystemDigitalOutputBits::bits24;
        auto expected = axk::encode_system_file(file).value();
        expected[0x68c] = std::byte{1};
        const auto result = axk::patch_system_playback(file, patch, axk::ASeriesModel::a4000);
        ASSERT_TRUE(result);
        EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
        EXPECT_EQ(axk::decode_system_playback(*result)->parameters.sequence_midi_port, axk::MidiPort::b);
    }
}

TEST(SystemPlayback, RejectsMalformedRetainedRecordsEvenForEmptyPatches) {
    for (bool native : {false, true}) {
        const auto model = native ? axk::ASeriesModel::a3000 : axk::ASeriesModel::a5000;
        const auto original = retained_playback(native, 0);
        for (unsigned defect = 0; defect < 5U; ++defect) {
            auto file = original;
            if (defect == 0U)
                file.system_bulk_bytes.pop_back();
            else if (defect == 1U)
                file.storage_revision = 2;
            else if (defect == 2U)
                file.system_header_bytes[0x0e] = std::byte{1};
            else if (defect == 3U)
                file.kind = static_cast<axk::SystemFileKind>(255);
            else
                file.system_header_bytes.clear();
            EXPECT_FALSE(axk::decode_system_playback(file));
            EXPECT_FALSE(axk::patch_system_playback(file, {}, model));
        }
    }
}
