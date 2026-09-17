#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>

#include <gtest/gtest.h>

#include "axklib/system_file.hpp"

namespace {

axk::DecodedSystemFile recording_file(bool native) {
    axk::DecodedSystemFile file;
    file.kind = native ? axk::SystemFileKind::a3000_system : axk::SystemFileKind::a4000_a5000_system2;
    file.system_bulk_bytes.resize(native ? 0x348U : 0xfe0U);
    constexpr std::array<std::uint8_t, 27> defaults{1,   1, 1, 0, 1,   1,   0,    20,   7, 1, 0, 127, 60, 0,
                                                    255, 1, 1, 0, 100, 100, 0x2e, 0xe0, 4, 0, 0, 36,  0};
    const auto offset = native ? 0x188U : 0x398U;
    std::ranges::transform(defaults, file.system_bulk_bytes.begin() + offset,
                           [](auto value) { return static_cast<std::byte>(value); });
    return file;
}

std::span<std::byte> configuration(axk::DecodedSystemFile &file) {
    const auto native = file.kind == axk::SystemFileKind::a3000_system;
    return std::span{file.system_bulk_bytes}.subspan(native ? 0x188U : 0x398U, native ? 34U : 56U);
}

} // namespace

TEST(SystemRecording, ReadsSavedScalarFieldsWithoutNormalizingTheSource) {
    for (const auto native : {false, true}) {
        const auto file = recording_file(native);
        const auto before = file;
        const auto decoded = axk::decode_system_recording(file);
        ASSERT_TRUE(decoded);
        const auto &p = decoded->parameters;
        EXPECT_EQ(p.record_type, 1);
        EXPECT_EQ(p.stereo, true);
        EXPECT_EQ(p.input, 1);
        EXPECT_EQ(p.frequency_selection, 0);
        EXPECT_EQ(p.pre_trigger_time, 1);
        EXPECT_EQ(p.start_trigger, 1);
        EXPECT_EQ(p.stop_trigger, 0);
        EXPECT_EQ(p.start_edge_level, 20);
        EXPECT_EQ(p.stop_edge_level, 7);
        EXPECT_EQ(p.map_destination, 1);
        EXPECT_EQ(p.key_low, 0);
        EXPECT_EQ(p.key_high, 127);
        EXPECT_EQ(p.original_key, 60);
        EXPECT_EQ(p.auto_normalize, false);
        EXPECT_EQ(p.external_scsi_id, -1);
        EXPECT_EQ(p.external_track, 1);
        EXPECT_EQ(p.external_index, 1);
        EXPECT_EQ(p.monitor_output, 0);
        EXPECT_EQ(p.monitor_level, 100);
        EXPECT_EQ(p.click_level, 100);
        EXPECT_EQ(p.click_tempo_hundredths, 12000);
        EXPECT_EQ(p.click_beat, 4);
        EXPECT_EQ(p.monitor_enabled, false);
        EXPECT_EQ(p.map_auto, false);
        EXPECT_EQ(p.map_original_key, 36);
        EXPECT_EQ(p.map_all_keys, false);
        EXPECT_EQ(p.ad_input_gain.has_value(), !native);
        EXPECT_EQ(file, before);
        EXPECT_EQ(decoded->raw_bytes.size(), native ? 154U : 176U);
    }
}

TEST(SystemRecording, PreservesGenerationSpecificDomainsAndKeySentinels) {
    for (const auto native : {false, true}) {
        auto file = recording_file(native);
        auto bytes = configuration(file);
        bytes[0] = std::byte{3};
        bytes[2] = std::byte{4};
        bytes[3] = std::byte{6};
        bytes[10] = std::byte{255};
        bytes[11] = std::byte{128};
        bytes[15] = std::byte{255};
        bytes[16] = std::byte{100};
        const auto decoded = axk::decode_system_recording(file);
        ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded->parameters.record_type.has_value(), !native);
        EXPECT_EQ(decoded->parameters.external_track.has_value(), native);
        EXPECT_EQ(decoded->parameters.external_index.has_value(), native);
        EXPECT_EQ(decoded->parameters.key_low, -1);
        EXPECT_EQ(decoded->parameters.key_high, 128);
        // Reading records stored values even when the input would constrain an edit.
        EXPECT_EQ(decoded->parameters.input, 4);
        EXPECT_EQ(decoded->parameters.frequency_selection, 6);
    }
}

TEST(SystemRecording, InvalidLeavesAndReservedBytesRemainLossless) {
    auto file = recording_file(false);
    auto bytes = configuration(file);
    std::ranges::fill(bytes, std::byte{0xa5});
    const auto decoded = axk::decode_system_recording(file);
    ASSERT_TRUE(decoded);
    const auto &p = decoded->parameters;
    EXPECT_FALSE(p.record_type);
    EXPECT_FALSE(p.stereo);
    EXPECT_FALSE(p.input);
    EXPECT_FALSE(p.frequency_selection);
    EXPECT_FALSE(p.key_low);
    EXPECT_FALSE(p.external_scsi_id);
    EXPECT_FALSE(p.external_track);
    EXPECT_FALSE(p.external_index);
    EXPECT_FALSE(p.click_tempo_hundredths);
    EXPECT_FALSE(p.ad_input_gain);
    EXPECT_TRUE(std::ranges::equal(bytes, std::span{decoded->raw_bytes}.subspan(120U)));
}

TEST(SystemRecording, ThreeEffectsUseGenerationSpecificTypeLocationsAndSharedDomains) {
    for (const auto native : {false, true}) {
        auto file = recording_file(native);
        for (std::size_t slot = 0; slot < 3U; ++slot) {
            const auto offset = (native ? 0x110U : 0x320U) + slot * 40U;
            auto effect = std::span{file.system_bulk_bytes}.subspan(offset, 40U);
            effect[0] = std::byte{1};
            effect[1] = static_cast<std::byte>(80U + slot);
            effect[2] = std::byte{127};
            effect[3] = std::byte{193};
            effect[4] = std::byte{5};
            effect[5] = std::byte{130};
            effect[6] = native ? std::byte{255} : std::byte{54};
            effect[7] = native ? std::byte{54} : std::byte{255};
        }
        const auto decoded = axk::decode_system_recording(file);
        ASSERT_TRUE(decoded);
        ASSERT_EQ(decoded->effects.size(), 3U);
        for (std::size_t slot = 0; slot < 3U; ++slot) {
            const auto &effect = decoded->effects[slot];
            EXPECT_EQ(effect.type, 54);
            EXPECT_EQ(effect.enabled, true);
            EXPECT_EQ(effect.input_level, 80U + slot);
            EXPECT_EQ(effect.pan, -63);
            EXPECT_EQ(effect.width, -126);
            EXPECT_EQ(effect.destination, 5);
        }
        const auto offset = native ? 0x110U : 0x320U;
        EXPECT_TRUE(std::ranges::equal(decoded->raw_bytes,
                                       std::span{file.system_bulk_bytes}.subspan(offset, native ? 154U : 176U)));
    }
}

TEST(SystemRecording, RejectsUnsupportedOrIncompleteLayouts) {
    for (const auto native : {false, true}) {
        auto file = recording_file(native);
        file.storage_revision = native ? 1U : 2U;
        EXPECT_FALSE(axk::decode_system_recording(file));
        file.storage_revision = 0;
        file.system_bulk_bytes.pop_back();
        EXPECT_FALSE(axk::decode_system_recording(file));
    }
    auto file = recording_file(false);
    file.storage_revision = 1;
    EXPECT_TRUE(axk::decode_system_recording(file));
    file.kind = static_cast<axk::SystemFileKind>(255);
    EXPECT_FALSE(axk::decode_system_recording(file));
}

TEST(SystemRecording, ChecksEveryByteScalarAcrossItsCompleteStorageDomain) {
    using Parameters = axk::SystemRecordingParameters;
    struct Field {
        std::size_t offset;
        std::optional<std::uint8_t> Parameters::*member;
        unsigned minimum;
        unsigned maximum;
    };
    constexpr std::array fields{
        Field{0x00, &Parameters::record_type, 0, 3},         Field{0x02, &Parameters::input, 0, 4},
        Field{0x03, &Parameters::frequency_selection, 0, 6}, Field{0x04, &Parameters::pre_trigger_time, 0, 5},
        Field{0x05, &Parameters::start_trigger, 0, 1},       Field{0x06, &Parameters::stop_trigger, 0, 1},
        Field{0x07, &Parameters::start_edge_level, 0, 63},   Field{0x08, &Parameters::stop_edge_level, 0, 63},
        Field{0x0b, &Parameters::key_high, 0, 128},          Field{0x0f, &Parameters::external_track, 1, 99},
        Field{0x10, &Parameters::external_index, 1, 99},     Field{0x11, &Parameters::monitor_output, 0, 5},
        Field{0x12, &Parameters::monitor_level, 0, 127},     Field{0x13, &Parameters::click_level, 0, 127},
        Field{0x16, &Parameters::click_beat, 1, 15},         Field{0x19, &Parameters::map_original_key, 0, 127},
        Field{0x33, &Parameters::ad_input_gain, 0, 1},
    };
    struct SignedField {
        std::size_t offset;
        std::optional<std::int8_t> Parameters::*member;
        int minimum;
        int maximum;
    };
    constexpr std::array signed_fields{
        SignedField{0x09, &Parameters::map_destination, 0, 2},
        SignedField{0x0a, &Parameters::key_low, -1, 127},
        SignedField{0x0c, &Parameters::original_key, 0, 127},
        SignedField{0x0e, &Parameters::external_scsi_id, -1, 7},
    };
    struct BooleanField {
        std::size_t offset;
        std::optional<bool> Parameters::*member;
    };
    constexpr std::array boolean_fields{
        BooleanField{0x01, &Parameters::stereo},          BooleanField{0x0d, &Parameters::auto_normalize},
        BooleanField{0x17, &Parameters::monitor_enabled}, BooleanField{0x18, &Parameters::map_auto},
        BooleanField{0x1a, &Parameters::map_all_keys},
    };
    for (const auto native : {false, true}) {
        for (unsigned raw = 0; raw < 256U; ++raw) {
            auto file = recording_file(native);
            std::ranges::fill(configuration(file), static_cast<std::byte>(raw));
            const auto decoded = axk::decode_system_recording(file);
            ASSERT_TRUE(decoded);
            for (const auto &field : fields) {
                const auto &value = decoded->parameters.*field.member;
                const auto maximum = native && field.offset == 0                            ? 2U
                                     : native && (field.offset == 15 || field.offset == 16) ? 255U
                                                                                            : field.maximum;
                const auto valid = !(native && field.offset == 0x33) && raw >= field.minimum && raw <= maximum;
                ASSERT_EQ(value.has_value(), valid) << field.offset << ": " << raw;
                if (value)
                    EXPECT_EQ(*value, raw);
            }
            const auto signed_raw = raw < 128U ? static_cast<int>(raw) : static_cast<int>(raw) - 256;
            for (const auto &field : signed_fields) {
                const auto &value = decoded->parameters.*field.member;
                ASSERT_EQ(value.has_value(), signed_raw >= field.minimum && signed_raw <= field.maximum);
                if (value)
                    EXPECT_EQ(*value, signed_raw);
            }
            for (const auto &field : boolean_fields) {
                const auto &value = decoded->parameters.*field.member;
                ASSERT_EQ(value.has_value(), raw <= 1U);
                if (value)
                    EXPECT_EQ(*value, raw == 1U);
            }
        }
    }
}

TEST(SystemRecording, ClickTempoUsesBigEndianHundredthsAndExactBounds) {
    for (const auto native : {false, true}) {
        for (const auto value : {0U, 7999U, 8000U, 12000U, 15999U, 16000U, 65535U}) {
            auto file = recording_file(native);
            auto bytes = configuration(file);
            bytes[0x14] = static_cast<std::byte>(value >> 8U);
            bytes[0x15] = static_cast<std::byte>(value & 255U);
            const auto decoded = axk::decode_system_recording(file);
            ASSERT_TRUE(decoded);
            const auto &tempo = decoded->parameters.click_tempo_hundredths;
            ASSERT_EQ(tempo.has_value(), value >= 8000U && value <= 15999U);
            if (tempo)
                EXPECT_EQ(*tempo, value);
        }
    }
}
