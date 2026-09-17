#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/system_file.hpp"

namespace {

axk::DecodedSystemFile retained_midi(bool native, std::uint8_t revision, unsigned value = 0xa5U) {
    const auto size = native ? 0x400U : 0x1000U;
    std::vector<std::byte> bytes(axk::current_record_envelope_size + size, static_cast<std::byte>(value));
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

constexpr auto models = std::array{axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000};

} // namespace

TEST(SystemMidi, ChangesOnlyRequestedBytesIncludingExplicitFalseAcrossModelsAndRevisions) {
    for (const auto model : models) {
        const bool native = model == axk::ASeriesModel::a3000;
        for (std::uint8_t revision = 0; revision <= (native ? 0U : 1U); ++revision) {
            const auto file = retained_midi(native, revision);
            const auto before = axk::encode_system_file(file).value();
            axk::SystemMidiParameters patch;
            patch.bulk_protect = true;
            patch.aftertouch_disabled = false;
            patch.control_change_disabled = true;
            patch.pitch_bend_disabled = false;
            patch.device_number = 17;
            if (!native)
                patch.sysex_receive_port =
                    model == axk::ASeriesModel::a5000 ? axk::SystemSysexReceivePort::b : axk::SystemSysexReceivePort::a;
            const auto result = axk::patch_system_midi(file, patch, model);
            ASSERT_TRUE(result);
            auto expected = before;
            const std::size_t offset = native ? 0x208U : 0x42cU;
            expected[offset + 2U] = std::byte{1};
            expected[offset + 3U] = std::byte{};
            expected[offset + 4U] = std::byte{1};
            expected[offset + 5U] = std::byte{};
            expected[offset + 7U] = std::byte{17};
            if (!native)
                expected[offset + 8U] = static_cast<std::byte>(*patch.sysex_receive_port);
            EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
            EXPECT_EQ(axk::decode_system_midi(*result)->parameters, patch);
            EXPECT_EQ(axk::encode_system_file(file).value(), before);
            EXPECT_EQ(axk::patch_system_midi(*result, patch, model).value(), *result);
        }
    }
}

TEST(SystemMidi, ReadsEveryStoredValueWithoutNormalizationAndRetainsTheCompleteBlock) {
    for (bool native : {false, true}) {
        for (std::uint8_t revision = 0; revision <= (native ? 0U : 1U); ++revision) {
            for (unsigned value = 0; value < 256U; ++value) {
                const auto file = retained_midi(native, revision, value);
                const auto before = axk::encode_system_file(file).value();
                const auto decoded = axk::decode_system_midi(file);
                ASSERT_TRUE(decoded);
                EXPECT_EQ(decoded->raw_bytes, std::vector<std::byte>(native ? 8U : 9U, static_cast<std::byte>(value)));
                for (const auto field :
                     {decoded->parameters.bulk_protect, decoded->parameters.aftertouch_disabled,
                      decoded->parameters.control_change_disabled, decoded->parameters.pitch_bend_disabled}) {
                    EXPECT_EQ(field.has_value(), value <= 1U);
                    if (field)
                        EXPECT_EQ(*field, value != 0U);
                }
                EXPECT_EQ(decoded->parameters.device_number.has_value(), value <= 17U);
                if (decoded->parameters.device_number)
                    EXPECT_EQ(*decoded->parameters.device_number, value);
                EXPECT_EQ(decoded->parameters.sysex_receive_port.has_value(), !native && value <= 1U);
                if (decoded->parameters.sysex_receive_port)
                    EXPECT_EQ(static_cast<unsigned>(*decoded->parameters.sysex_receive_port), value);
                EXPECT_EQ(axk::encode_system_file(file).value(), before);
            }
        }
    }
}

TEST(SystemMidi, EachBooleanEditPreservesEveryOtherByteAndEmptyPatchesAreLossless) {
    const auto fields = std::array{
        &axk::SystemMidiParameters::bulk_protect, &axk::SystemMidiParameters::aftertouch_disabled,
        &axk::SystemMidiParameters::control_change_disabled, &axk::SystemMidiParameters::pitch_bend_disabled};
    for (auto model : models) {
        const bool native = model == axk::ASeriesModel::a3000;
        for (std::uint8_t revision = 0; revision <= (native ? 0U : 1U); ++revision) {
            const auto file = retained_midi(native, revision);
            const auto before = axk::encode_system_file(file).value();
            EXPECT_EQ(axk::patch_system_midi(file, {}, model).value(), file);
            for (std::size_t i = 0; i < fields.size(); ++i) {
                for (bool value : {false, true}) {
                    axk::SystemMidiParameters patch;
                    patch.*fields[i] = value;
                    const auto updated = axk::patch_system_midi(file, patch, model);
                    ASSERT_TRUE(updated);
                    auto expected = before;
                    expected[(native ? 0x208U : 0x42cU) + 2U + i] = static_cast<std::byte>(value);
                    EXPECT_EQ(axk::encode_system_file(*updated).value(), expected);
                }
            }
        }
    }
}

TEST(SystemMidi, ChecksEveryDeviceAndPortRequestAgainstItsModelWithoutChangingUnrequestedInvalidValues) {
    for (auto model : models) {
        const bool native = model == axk::ASeriesModel::a3000;
        for (std::uint8_t revision = 0; revision <= (native ? 0U : 1U); ++revision) {
            const auto file = retained_midi(native, revision);
            const auto before = axk::encode_system_file(file).value();
            for (unsigned value = 0; value < 256U; ++value) {
                for (bool port : {false, true}) {
                    SCOPED_TRACE(static_cast<unsigned>(model));
                    SCOPED_TRACE(value);
                    axk::SystemMidiParameters patch;
                    if (port)
                        patch.sysex_receive_port = static_cast<axk::SystemSysexReceivePort>(value);
                    else
                        patch.device_number = static_cast<std::uint8_t>(value);
                    const auto updated = axk::patch_system_midi(file, patch, model);
                    const bool valid =
                        port ? !native && value <= (model == axk::ASeriesModel::a5000 ? 1U : 0U) : value <= 17U;
                    ASSERT_EQ(updated.has_value(), valid);
                    if (updated) {
                        auto expected = before;
                        expected[(native ? 0x208U : 0x42cU) + (port ? 8U : 7U)] = static_cast<std::byte>(value);
                        EXPECT_EQ(axk::encode_system_file(*updated).value(), expected);
                    } else {
                        EXPECT_EQ(updated.error().code, axk::ErrorCode::invalid_argument);
                        axk::SystemFilePatch mixed;
                        mixed.global.master_fine_tune = 7;
                        mixed.midi = patch;
                        EXPECT_FALSE(axk::patch_system_file(file, mixed, model));
                    }
                    EXPECT_EQ(axk::encode_system_file(file).value(), before);
                }
            }
        }
    }
}

TEST(SystemMidi, RejectsMalformedRecordsAndMismatchedModelsEvenForEmptyPatches) {
    const auto file = retained_midi(false, 1);
    for (const auto model : {axk::ASeriesModel::a3000, static_cast<axk::ASeriesModel>(255)})
        EXPECT_FALSE(axk::patch_system_midi(file, {}, model));
    EXPECT_FALSE(axk::patch_system_midi(retained_midi(true, 0), {}, axk::ASeriesModel::a5000));
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
        EXPECT_FALSE(axk::decode_system_midi(malformed));
        EXPECT_FALSE(axk::patch_system_midi(malformed, {}, axk::ASeriesModel::a5000));
    }
}
