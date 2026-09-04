#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/tx16w.hpp"
#include "axklib/tx16w_a_series.hpp"

namespace {

void put_text(std::vector<std::byte> &bytes, std::size_t offset, std::string_view text, std::size_t width) {
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), static_cast<std::ptrdiff_t>(width),
                std::byte{' '});
    for (std::size_t index = 0U; index < std::min(width, text.size()); ++index)
        bytes[offset + index] = static_cast<std::byte>(text[index]);
}

std::vector<std::byte> tx_file(std::size_t size) {
    std::vector<std::byte> bytes(size);
    put_text(bytes, 0U, "LM8953", 6U);
    return bytes;
}

} // namespace

TEST(Tx16wWave, DecodesNativeHeaderAndPackedSignedPcm) {
    auto bytes = tx_file(38U);
    bytes[22] = std::byte{0x49};
    bytes[23] = std::byte{0x03};
    bytes[24] = std::byte{0x02};
    bytes[27] = std::byte{0x02};
    bytes[32] = std::byte{0x7f};
    bytes[33] = std::byte{0xf0};
    bytes[34] = std::byte{0x80};
    bytes[35] = std::byte{0x00};
    bytes[36] = std::byte{0x1f};
    bytes[37] = std::byte{0xff};

    const auto wave = axk::tx16w::decode_wave(bytes, "A0.W02");

    ASSERT_TRUE(wave) << wave.error().message;
    EXPECT_EQ(wave->name, "A0.W02");
    EXPECT_EQ(wave->sample_rate, 16667U);
    EXPECT_EQ(wave->attack_frames, 2U);
    EXPECT_EQ(wave->repeat_frames, 2U);
    EXPECT_TRUE(wave->looped);
    ASSERT_TRUE(wave->native_slot);
    EXPECT_EQ(*wave->native_slot, 1U);
    EXPECT_EQ(wave->pcm, (std::vector<std::int16_t>{32752, -32768, 16, -16}));
}

TEST(Tx16wWave, UsesLogicalFrameLengthAndIgnoresFatAllocationSlack) {
    auto bytes = tx_file(163U);
    bytes[22] = std::byte{0xc9};
    bytes[23] = std::byte{0x00};
    bytes[24] = std::byte{0x01};
    bytes[27] = std::byte{0x01};
    bytes[26] = std::byte{0x10};
    bytes[29] = std::byte{0x00};
    bytes[32] = std::byte{0x7f};
    bytes[33] = std::byte{0xf0};
    bytes[34] = std::byte{0x80};
    std::fill(bytes.begin() + 35, bytes.end(), std::byte{0xa5});

    const auto wave = axk::tx16w::decode_wave(bytes, "RATE.W01");
    ASSERT_TRUE(wave) << wave.error().message;
    EXPECT_EQ(wave->sample_rate, 50000U);
    EXPECT_FALSE(wave->looped);
    EXPECT_EQ(wave->pcm, (std::vector<std::int16_t>{32752, -32768}));
}

TEST(Tx16wWave, DiscardsTheUnusedPartnerForAnOddLogicalFrameCount) {
    auto bytes = tx_file(72U);
    bytes[22] = std::byte{0xc9};
    bytes[23] = std::byte{0x01};
    bytes[24] = std::byte{0x03};
    bytes[32] = std::byte{0x00};
    bytes[33] = std::byte{0x10};
    bytes[34] = std::byte{0x02};
    bytes[35] = std::byte{0x03};
    bytes[36] = std::byte{0x40};
    bytes[37] = std::byte{0x05};
    std::fill(bytes.begin() + 38, bytes.end(), std::byte{0x5a});

    const auto wave = axk::tx16w::decode_wave(bytes, "ODD.W01");

    ASSERT_TRUE(wave) << wave.error().message;
    EXPECT_EQ(wave->pcm, (std::vector<std::int16_t>{16, 512, 832}));
}

TEST(Tx16wWave, RejectsPayloadShorterThanItsDeclaredLogicalFrameCount) {
    auto bytes = tx_file(35U);
    bytes[22] = std::byte{0xc9};
    bytes[23] = std::byte{0x01};
    bytes[24] = std::byte{0x03};

    const auto malformed = axk::tx16w::decode_wave(bytes, "BROKEN.W01");

    ASSERT_FALSE(malformed);
    EXPECT_EQ(malformed.error().code, axk::ErrorCode::container_truncated);
}

TEST(Tx16wWave, DefaultsUnknownRateEncodingWithExplicitSourceMetadata) {
    auto bytes = tx_file(35U);
    bytes[22] = std::byte{0xc9};
    bytes[23] = std::byte{0x7f};
    bytes[24] = std::byte{0x02};
    bytes[26] = std::byte{0x02};
    bytes[29] = std::byte{0x18};

    const auto wave = axk::tx16w::decode_wave(bytes, "UNKNOWN.W01");

    ASSERT_TRUE(wave) << wave.error().message;
    EXPECT_EQ(wave->sample_rate, 33'333U);
    EXPECT_EQ(wave->sample_rate_source, axk::tx16w::SampleRateSource::defaulted_33333);
    EXPECT_EQ(wave->native_rate_code, 0x7fU);
    EXPECT_EQ(wave->native_attack_rate_marker, 0x02U);
    EXPECT_EQ(wave->native_repeat_rate_marker, 0x18U);
}

TEST(Tx16wSetup, DecodesWaveReferencesVoiceRegionsAndTimbres) {
    auto setup = tx_file(1536U);
    auto voices = tx_file(8704U);
    auto performances = tx_file(5120U);
    put_text(setup, 0xf0U + 7U, "PIANO", 8U);
    put_text(performances, 0x10U + 0x78U, "Concert Grand Piano", 20U);

    constexpr std::size_t voice_base = 0x10U;
    voices[voice_base] = std::byte{0x00};
    voices[voice_base + 1U] = std::byte{0x24};
    voices[voice_base + 2U] = std::byte{0x8e};
    voices[voice_base + 3U] = std::byte{0x09};
    voices[voice_base + 4U] = std::byte{0x01};
    voices[voice_base + 5U] = std::byte{0x3d};
    voices[voice_base + 6U] = std::byte{0x54};
    voices[voice_base + 7U] = std::byte{0x03};
    for (std::size_t offset = 8U; offset < 32U * 4U; offset += 4U) {
        voices[voice_base + offset + 1U] = std::byte{0xff};
        voices[voice_base + offset + 2U] = std::byte{0xff};
    }
    put_text(voices, voice_base + 0x86U, "Grand", 10U);

    constexpr std::size_t performance_base = 0x10U;
    performances[performance_base + 2U] = std::byte{0x10};
    performances[performance_base + 18U] = std::byte{0x00};
    performances[performance_base + 34U] = std::byte{0x03};
    performances[performance_base + 50U] = std::byte{0x02};
    performances[performance_base + 66U] = std::byte{0x63};
    performances[performance_base + 82U] = std::byte{0xfb};
    performances[performance_base + 98U] = std::byte{0x07};

    constexpr std::size_t timbre_base = 0x1250U;
    voices[timbre_base] = std::byte{0x00};
    voices[timbre_base + 1U] = std::byte{0x3c};
    put_text(voices, timbre_base + 46U, "Piano Low", 10U);
    voices[timbre_base + 0x38U] = std::byte{0x00};
    voices[timbre_base + 0x38U + 1U] = std::byte{0x48};
    put_text(voices, timbre_base + 0x38U + 46U, "Piano High", 10U);

    const auto decoded = axk::tx16w::decode_native_setup(setup, voices, performances, "KBD-1");

    ASSERT_TRUE(decoded) << decoded.error().message;
    ASSERT_EQ(decoded->waves.size(), 1U);
    EXPECT_EQ(decoded->waves.front().slot, 0U);
    EXPECT_EQ(decoded->waves.front().name, "PIANO");
    ASSERT_EQ(decoded->performances.size(), 1U);
    EXPECT_EQ(decoded->performances.front().name, "Concert Grand Piano");
    EXPECT_EQ(decoded->performances.front().voices[0].receive_channel, 0x10U);
    EXPECT_EQ(decoded->performances.front().voices[0].voice_slot, 0U);
    EXPECT_EQ(decoded->performances.front().voices[0].alternative_group, 3U);
    EXPECT_EQ(decoded->performances.front().voices[0].audio_output, 2U);
    EXPECT_EQ(decoded->performances.front().voices[0].volume, 99U);
    EXPECT_EQ(decoded->performances.front().voices[0].detune, -5);
    EXPECT_EQ(decoded->performances.front().voices[0].transpose, 7);
    ASSERT_EQ(decoded->voices.size(), 1U);
    EXPECT_EQ(decoded->voices.front().name, "Grand");
    ASSERT_EQ(decoded->voices.front().regions.size(), 2U);
    EXPECT_EQ(decoded->voices.front().regions[0].low_key_number, 0x24U);
    EXPECT_EQ(decoded->voices.front().regions[0].high_key_number, 0x8eU);
    EXPECT_EQ(decoded->voices.front().regions[0].fade, 9U);
    EXPECT_EQ(decoded->voices.front().regions[1].timbre_slot, 1U);
    ASSERT_EQ(decoded->timbres.size(), 2U);
    EXPECT_EQ(decoded->timbres[0].name, "Piano Low");
    EXPECT_EQ(decoded->timbres[0].root_key_number, 0x3cU);
    EXPECT_EQ(decoded->timbres[1].name, "Piano High");
}

TEST(Tx16wSetup, DecodesVersion0200LayoutNativePaddingAndSelectorFlags) {
    auto setup = tx_file(1536U);
    auto voices = tx_file(8704U);
    auto performances = tx_file(5120U);
    put_text(setup, 7U, "0200", 4U);
    put_text(setup, 0xf2U + 7U, "TONE", 8U);

    constexpr std::size_t voice_base = 0x10U;
    voices[voice_base] = std::byte{0x41};
    voices[voice_base + 1U] = std::byte{0x24};
    voices[voice_base + 2U] = std::byte{0x60};
    for (std::size_t region = 1U; region < 32U; ++region) {
        const auto offset = voice_base + region * 4U;
        voices[offset + 1U] = std::byte{0xff};
        voices[offset + 2U] = std::byte{0x00};
    }
    put_text(voices, voice_base + 0x86U, "Filtered ", 10U);
    voices[voice_base + 0x86U + 9U] = std::byte{0x18};

    constexpr std::size_t timbre_base = 0x1250U + 0x38U;
    voices[timbre_base] = std::byte{0x00};
    voices[timbre_base + 1U] = std::byte{0x4c};
    put_text(voices, timbre_base + 46U, " HC-2    ", 10U);
    voices[timbre_base + 55U] = std::byte{0x98};

    constexpr std::size_t performance_base = 0x10U;
    put_text(performances, performance_base + 0x78U, " E.Bass", 20U);

    const auto decoded = axk::tx16w::decode_native_setup(setup, voices, performances, "0200 SET");

    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->layout, axk::tx16w::NativeSetupLayout::version_0200);
    EXPECT_FALSE(decoded->write_protected);
    ASSERT_EQ(decoded->waves.size(), 1U);
    EXPECT_EQ(decoded->waves.front().name, "TONE");
    ASSERT_EQ(decoded->performances.size(), 1U);
    EXPECT_EQ(decoded->performances.front().name, "E.Bass");
    ASSERT_EQ(decoded->voices.size(), 1U);
    EXPECT_EQ(decoded->voices.front().name, "Filtered");
    ASSERT_EQ(decoded->voices.front().regions.size(), 1U);
    EXPECT_EQ(decoded->voices.front().regions.front().timbre_slot, 1U);
    EXPECT_EQ(decoded->voices.front().regions.front().native_timbre_selector_flags, 0x40U);
    ASSERT_EQ(decoded->timbres.size(), 1U);
    EXPECT_EQ(decoded->timbres.front().name, "HC-2");
}

TEST(Tx16wSetup, RecognizesProtectedLegacyLayoutAndRejectsUnknownLayouts) {
    auto setup = tx_file(1536U);
    auto voices = tx_file(8704U);
    setup[6] = std::byte{0x01};
    put_text(setup, 0xf0U + 7U, "TONE", 8U);

    const auto legacy = axk::tx16w::decode_native_setup(setup, voices, {}, "PROTECTED");

    ASSERT_TRUE(legacy) << legacy.error().message;
    EXPECT_EQ(legacy->layout, axk::tx16w::NativeSetupLayout::legacy);
    EXPECT_TRUE(legacy->write_protected);
    ASSERT_EQ(legacy->waves.size(), 1U);
    EXPECT_EQ(legacy->waves.front().name, "TONE");

    setup[7] = std::byte{'X'};
    const auto unsupported = axk::tx16w::decode_native_setup(setup, voices, {}, "UNKNOWN");
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.error().code, axk::ErrorCode::unsupported_profile);
}

TEST(Tx16wInspection, MarksCycloneAuxiliaryFilesAsOmittedWithoutRejectingNativeData) {
    std::vector<axk::tx16w::SourceFile> files;
    files.push_back({"KIT.S01", tx_file(1536U), {}});
    files.push_back({"KIT.V01", tx_file(8704U), {}});
    files.push_back({"KIT.U01", tx_file(5120U), {}});
    files.push_back({"KIT.P01", tx_file(512U), {}});
    files.push_back({"KIT.R01", tx_file(512U), {}});
    files.push_back({"TONE.C01", tx_file(512U), {}});
    auto wave = tx_file(35U);
    wave[22] = std::byte{0xc9};
    wave[23] = std::byte{0x7f};
    wave[24] = std::byte{0x02};
    wave[26] = std::byte{0x02};
    wave[29] = std::byte{0x18};
    files.push_back({"KIT.W01", std::move(wave), {}});

    const auto inspection = axk::tx16w::inspect_files(files);

    ASSERT_TRUE(inspection) << inspection.error().message;
    EXPECT_EQ(inspection->profile, axk::tx16w::Profile::yamaha_native_with_auxiliary_files);
    ASSERT_EQ(inspection->setups.size(), 1U);
    ASSERT_EQ(inspection->unsupported_files.size(), 3U);
    EXPECT_EQ(inspection->unsupported_files.front(), "KIT.P01");
    EXPECT_EQ(inspection->unsupported_files.back(), "TONE.C01");
    ASSERT_EQ(inspection->notices.size(), 1U);
    EXPECT_EQ(inspection->notices.front().disposition, axk::tx16w::ParseNoticeDisposition::defaulted);
    EXPECT_EQ(inspection->notices.front().source_parameter, "sample_rate_encoding");

    const auto plan = axk::tx16w::a_series::plan_import(*inspection);
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_TRUE(std::ranges::any_of(plan->notices, [](const auto &notice) {
        return notice.disposition == axk::tx16w::a_series::MappingDisposition::omitted &&
               notice.source_parameter == "compressed_wave_payload" && notice.source_object == "TONE.C01";
    }));
}

TEST(Tx16wInspection, DecodesMultipleNativeSetupGroupsFromOneDiskSet) {
    std::vector<axk::tx16w::SourceFile> files;
    files.push_back({"FIRST.S01", tx_file(1536U), "disk.img"});
    files.push_back({"FIRST.V01", tx_file(8704U), "disk.img"});
    files.push_back({"SECOND.S02", tx_file(1536U), "disk.img"});
    files.push_back({"SECOND.V02", tx_file(8704U), "disk.img"});

    const auto inspection = axk::tx16w::inspect_files(files);

    ASSERT_TRUE(inspection) << inspection.error().message;
    ASSERT_EQ(inspection->setups.size(), 2U);
    EXPECT_EQ(inspection->setups[0].name, "FIRST");
    EXPECT_EQ(inspection->setups[1].name, "SECOND");
}

TEST(Tx16wInspection, ResolvesHierarchyWavesAcrossExplicitDiskSetMembers) {
    auto setup = tx_file(1536U);
    auto voices = tx_file(8704U);
    auto wave = tx_file(35U);
    put_text(setup, 0xf0U + 7U, "TONE", 8U);

    constexpr std::size_t voice_base = 0x10U;
    voices[voice_base] = std::byte{0x00};
    voices[voice_base + 1U] = std::byte{0x24};
    voices[voice_base + 2U] = std::byte{0x60};
    for (std::size_t region = 1U; region < 32U; ++region) {
        const auto offset = voice_base + region * 4U;
        voices[offset + 1U] = std::byte{0xff};
        voices[offset + 2U] = std::byte{0xff};
    }
    put_text(voices, voice_base + 0x86U, "Voice", 10U);
    constexpr std::size_t timbre_base = 0x1250U;
    voices[timbre_base] = std::byte{0x00};
    voices[timbre_base + 1U] = std::byte{0x4c};
    put_text(voices, timbre_base + 46U, "Member", 10U);

    wave[22] = std::byte{0xc9};
    wave[23] = std::byte{0x00};
    wave[24] = std::byte{0x02};
    wave[32] = std::byte{0x00};
    wave[33] = std::byte{0x10};
    wave[34] = std::byte{0x02};

    const std::vector<axk::tx16w::SourceFile> files{{"BANK.S01", std::move(setup), "disk-a.img"},
                                                    {"BANK.V01", std::move(voices), "disk-a.img"},
                                                    {"TONE.W01", std::move(wave), "disk-b.img"}};

    const auto inspection = axk::tx16w::inspect_files(files);
    ASSERT_TRUE(inspection) << inspection.error().message;
    ASSERT_EQ(inspection->waves.size(), 1U);
    EXPECT_EQ(inspection->waves.front().source_member, "disk-b.img");

    const auto plan = axk::tx16w::a_series::plan_import(*inspection);
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_EQ(plan->programs.size(), 1U);
    EXPECT_EQ(plan->sample_banks.size(), 1U);
    EXPECT_EQ(plan->samples.size(), 1U);
    EXPECT_EQ(plan->wave_data.size(), 1U);
    EXPECT_FALSE(std::ranges::any_of(plan->notices, [](const auto &notice) {
        return notice.disposition == axk::tx16w::a_series::MappingDisposition::blocked;
    }));
}

TEST(Tx16wASeries, PlansRelationshipsAndReportsLossyParameterMappings) {
    axk::tx16w::Inspection inspection;
    inspection.waves.push_back({"C0.W01", 33'333U, 100U, 200U, true, {0, 16, -16}, {}, {}, {}, {}, {}, {}});
    axk::tx16w::NativeSetup setup;
    setup.name = "KBD-1";
    setup.waves.push_back({0U, "C0"});
    setup.timbres.push_back({0U, "Piano Low", 0U, 76U});
    setup.voices.push_back({0U, "Grand Voice", {{0U, 36U, 142U, 4U, 0x40U}}});
    axk::tx16w::Performance performance;
    performance.slot = 0U;
    performance.name = "Concert Grand Piano";
    performance.voices[0] = {16U, 0U, 0U, 1U, 99U, 0, 0};
    for (std::size_t index = 1U; index < performance.voices.size(); ++index)
        performance.voices[index] = {16U, 0U, 0U, 1U, 99U, 0, 0};
    setup.performances.push_back(std::move(performance));
    inspection.setups.push_back(std::move(setup));
    inspection.notices.push_back({axk::tx16w::ParseNoticeDisposition::defaulted, "C0.W01", "sample_rate_encoding",
                                  "Unknown native rate encoding defaults to 33,333 Hz"});
    axk::tx16w::a_series::TargetInventory target;
    target.occupied_program_slots = {1U, 3U};

    const auto plan = axk::tx16w::a_series::plan_import(inspection, target);

    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_EQ(plan->wave_data.size(), 1U);
    EXPECT_EQ(plan->wave_data[0].name, "C0");
    EXPECT_EQ(plan->wave_data[0].target_sample_rate, 32'000U);
    EXPECT_EQ(plan->wave_data[0].root_key, 60U);
    EXPECT_EQ(plan->wave_data[0].loop_start_frame, 96U);
    EXPECT_EQ(plan->wave_data[0].loop_length_frames, 192U);
    ASSERT_EQ(plan->samples.size(), 1U);
    EXPECT_EQ(plan->samples[0].waveform_id, "C0");
    EXPECT_EQ(plan->samples[0].parameters.root_key, 60U);
    EXPECT_EQ(plan->samples[0].parameters.key_low, 20U);
    EXPECT_EQ(plan->samples[0].parameters.key_high, 126U);
    ASSERT_EQ(plan->sample_banks.size(), 1U);
    EXPECT_EQ(plan->sample_banks[0].name, "Grand Voice");
    EXPECT_EQ(plan->sample_banks[0].member_samples, (std::vector<std::string>{"Piano Low"}));
    ASSERT_EQ(plan->programs.size(), 1U);
    EXPECT_EQ(plan->programs[0].number, 2U);
    EXPECT_EQ(plan->programs[0].name, "Concert");
    ASSERT_EQ(plan->programs[0].assignments.size(), 1U);
    EXPECT_EQ(plan->programs[0].assignments[0].target_name, "Grand Voice");
    EXPECT_EQ(plan->programs[0].assignments[0].receive_mode, axk::ProgramReceiveMode::sample);
    EXPECT_TRUE(std::ranges::any_of(plan->notices, [](const auto &notice) {
        return notice.disposition == axk::tx16w::a_series::MappingDisposition::omitted &&
               notice.source_parameter == "fade";
    }));
    EXPECT_TRUE(std::ranges::any_of(plan->notices, [](const auto &notice) {
        return notice.disposition == axk::tx16w::a_series::MappingDisposition::approximated &&
               notice.source_parameter == "sample_rate";
    }));
    EXPECT_TRUE(std::ranges::any_of(plan->notices, [](const auto &notice) {
        return notice.disposition == axk::tx16w::a_series::MappingDisposition::omitted &&
               notice.source_parameter == "performance_voice_controls";
    }));
    EXPECT_TRUE(std::ranges::any_of(plan->notices, [](const auto &notice) {
        return notice.disposition == axk::tx16w::a_series::MappingDisposition::defaulted &&
               notice.source_parameter == "sample_rate_encoding";
    }));
    EXPECT_TRUE(std::ranges::any_of(plan->notices, [](const auto &notice) {
        return notice.disposition == axk::tx16w::a_series::MappingDisposition::omitted &&
               notice.source_parameter == "timbre_selector_flags";
    }));
}

TEST(Tx16wASeries, ResolvesSameNamedWavesByNativeFileSlot) {
    axk::tx16w::Inspection inspection;
    inspection.waves.push_back({"DUP.W03", 50'000U, 0U, 2U, false, {0, 16}, {}, {}, {}, {}, {}, std::uint8_t{2U}});
    inspection.waves.push_back({"DUP.W11", 50'000U, 0U, 2U, false, {0, 32}, {}, {}, {}, {}, {}, std::uint8_t{10U}});
    axk::tx16w::NativeSetup setup;
    setup.name = "DUPLICATES";
    setup.waves.push_back({2U, "DUP"});
    setup.waves.push_back({10U, "DUP"});
    setup.timbres.push_back({0U, "First", 2U, 76U});
    setup.timbres.push_back({1U, "Second", 10U, 76U});
    setup.voices.push_back({0U, "Both", {{0U, 16U, 79U, 0U, 0U}, {1U, 80U, 143U, 0U, 0U}}});
    inspection.setups.push_back(std::move(setup));

    const auto plan = axk::tx16w::a_series::plan_import(inspection);

    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_EQ(plan->wave_data.size(), 2U);
    EXPECT_EQ(plan->samples.size(), 2U);
    EXPECT_FALSE(std::ranges::any_of(plan->notices, [](const auto &notice) {
        return notice.disposition == axk::tx16w::a_series::MappingDisposition::blocked;
    }));
}

TEST(Tx16wASeries, PreservesPerformancesUpToTheNativeAssignmentCapacity) {
    axk::tx16w::Inspection inspection;
    inspection.waves.push_back({"TONE.W01", 50'000U, 0U, 2U, false, {0, 16}, {}, {}, {}, {}, {}, {}});
    axk::tx16w::NativeSetup setup;
    setup.name = "WIDE";
    setup.waves.push_back({0U, "TONE"});
    axk::tx16w::Performance performance;
    performance.name = "Wide Performance";
    for (std::uint8_t index = 0U; index < 12U; ++index) {
        setup.timbres.push_back({index, std::format("Member {}", index), 0U, 76U});
        setup.voices.push_back({index, std::format("Voice {}", index), {{index, 36U, 96U, 0U}}});
        performance.voices[index] = {16U, index, 0U, 0U, 99U, 0, 0};
    }
    for (std::uint8_t index = 12U; index < 16U; ++index)
        performance.voices[index] = {16U, 0U, 0U, 0U, 99U, 0, 0};
    setup.performances.push_back(std::move(performance));
    inspection.setups.push_back(std::move(setup));

    const auto plan = axk::tx16w::a_series::plan_import(inspection);

    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_EQ(plan->programs.size(), 1U);
    EXPECT_EQ(plan->programs.front().assignments.size(), 12U);
    ASSERT_EQ(plan->wave_data.size(), 1U);
    EXPECT_EQ(plan->wave_data.front().loop_mode, axk::AudioSamplerLoopMode::forward_one_shot);
    EXPECT_EQ(plan->wave_data.front().loop_start_frame, 0U);
    EXPECT_EQ(plan->wave_data.front().loop_length_frames, 0U);
    EXPECT_FALSE(std::ranges::any_of(plan->notices, [](const auto &notice) {
        return notice.disposition == axk::tx16w::a_series::MappingDisposition::blocked;
    }));
}

TEST(Tx16wASeries, PreservesVoiceReuseAcrossPrograms) {
    axk::tx16w::Inspection inspection;
    inspection.waves.push_back({"TONE.W01", 50'000U, 0U, 2U, false, {0, 16}, {}, {}, {}, {}, {}, {}});
    axk::tx16w::NativeSetup setup;
    setup.name = "SHARED";
    setup.waves.push_back({0U, "TONE"});
    setup.timbres.push_back({0U, "Member", 0U, 76U});
    setup.voices.push_back({0U, "Shared Voice", {{0U, 36U, 96U, 0U}}});
    axk::tx16w::Performance first;
    first.name = "First";
    first.voices[0] = {16U, 0U, 0U, 0U, 99U, 0, 0};
    axk::tx16w::Performance second;
    second.name = "Second";
    second.voices[0] = {16U, 0U, 0U, 0U, 99U, 0, 0};
    setup.performances = {std::move(first), std::move(second)};
    inspection.setups.push_back(std::move(setup));

    const auto plan = axk::tx16w::a_series::plan_import(inspection);

    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_EQ(plan->programs.size(), 2U);
    ASSERT_EQ(plan->programs[0].assignments.size(), 1U);
    ASSERT_EQ(plan->programs[1].assignments.size(), 1U);
    EXPECT_EQ(plan->programs[0].assignments[0].target_name, plan->programs[1].assignments[0].target_name);
    EXPECT_FALSE(std::ranges::any_of(plan->notices, [](const auto &notice) {
        return notice.disposition == axk::tx16w::a_series::MappingDisposition::blocked;
    }));
}

TEST(Tx16wASeries, BlocksIncompleteHierarchyButAllowsExplicitWaveDataOnlyImport) {
    axk::tx16w::Inspection inspection;
    inspection.waves.push_back({"HERE.W01", 50'000U, 0U, 2U, false, {0, 16}, {}, {}, {}, {}, {}, {}});
    axk::tx16w::NativeSetup setup;
    setup.name = "PARTIAL";
    setup.waves = {{0U, "HERE"}, {1U, "OTHER"}};
    setup.timbres = {{0U, "Present", 0U, 76U}, {1U, "Absent", 1U, 76U}, {2U, "Bad Key", 0U, 8U}};
    setup.voices = {{0U, "Mixed", {{0U, 36U, 96U, 0U}, {1U, 36U, 96U, 0U}, {2U, 36U, 96U, 0U}}}};
    inspection.setups.push_back(std::move(setup));

    const auto plan = axk::tx16w::a_series::plan_import(inspection, {}, axk::tx16w::ImportMode::hierarchy);

    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_TRUE(std::ranges::any_of(plan->notices, [](const auto &notice) {
        return notice.disposition == axk::tx16w::a_series::MappingDisposition::blocked;
    }));

    const auto wave_only = axk::tx16w::a_series::plan_import(inspection, {}, axk::tx16w::ImportMode::wave_data_only);
    ASSERT_TRUE(wave_only) << wave_only.error().message;
    EXPECT_EQ(wave_only->wave_data.size(), 1U);
    EXPECT_TRUE(wave_only->samples.empty());
    EXPECT_TRUE(wave_only->sample_banks.empty());
    EXPECT_TRUE(wave_only->programs.empty());
    EXPECT_FALSE(std::ranges::any_of(wave_only->notices, [](const auto &notice) {
        return notice.disposition == axk::tx16w::a_series::MappingDisposition::blocked;
    }));
    EXPECT_TRUE(std::ranges::any_of(plan->notices, [](const auto &notice) {
        return notice.disposition == axk::tx16w::a_series::MappingDisposition::blocked &&
               notice.source_parameter == "wave_slot";
    }));
    EXPECT_TRUE(std::ranges::any_of(plan->notices, [](const auto &notice) {
        return notice.disposition == axk::tx16w::a_series::MappingDisposition::omitted &&
               notice.source_parameter == "key_number";
    }));
}

TEST(Tx16wASeries, ResolvesFatUnderscoresAgainstNativeSpacePaddedWaveReferences) {
    axk::tx16w::Inspection inspection;
    inspection.waves.push_back({"BIG_EP.W01", 50'000U, 0U, 2U, false, {0, 16}, {}, {}, {}, {}, {}, {}});
    axk::tx16w::NativeSetup setup;
    setup.name = "MULTI";
    setup.waves = {{0U, "BIG EP"}};
    setup.timbres = {{0U, "Member", 0U, 76U}};
    setup.voices = {{0U, "Voice", {{0U, 36U, 96U, 0U}}}};
    inspection.setups.push_back(std::move(setup));

    const auto plan = axk::tx16w::a_series::plan_import(inspection);

    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_EQ(plan->wave_data.size(), 1U);
    EXPECT_FALSE(std::ranges::any_of(plan->notices, [](const auto &notice) {
        return notice.disposition == axk::tx16w::a_series::MappingDisposition::blocked;
    }));
}

TEST(Tx16wASeries, ResolvesTargetCollisionsAndSparseWaveSlots) {
    axk::tx16w::Inspection inspection;
    inspection.waves.push_back({"TONE.W01", 50'000U, 0U, 2U, false, {0, 16}, {}, {}, {}, {}, {}, {}});
    axk::tx16w::NativeSetup setup;
    setup.name = "SPARSE";
    setup.waves.push_back({7U, "TONE"});
    setup.timbres.push_back({3U, "Member", 7U, 76U});
    setup.voices.push_back({4U, "Voice", {{3U, 36U, 96U, 0U}}});
    setup.performances.push_back({0U, "Program", {{{16U, 4U, 0U, 0U, 99U, 0, 0}}}});
    inspection.setups.push_back(std::move(setup));
    axk::tx16w::a_series::TargetInventory target;
    target.wave_data_names = {"tone"};
    target.sample_names = {"MEMBER"};
    target.sample_bank_names = {"voice"};
    target.program_names = {"PROGRAM"};

    const auto plan = axk::tx16w::a_series::plan_import(inspection, target);

    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_EQ(plan->wave_data.size(), 1U);
    EXPECT_EQ(plan->wave_data[0].name, "TONE 2");
    ASSERT_EQ(plan->samples.size(), 1U);
    EXPECT_EQ(plan->samples[0].name, "Member 2");
    EXPECT_EQ(plan->samples[0].waveform_id, "TONE 2");
    ASSERT_EQ(plan->sample_banks.size(), 1U);
    EXPECT_EQ(plan->sample_banks[0].name, "Voice 2");
    ASSERT_EQ(plan->programs.size(), 1U);
    EXPECT_EQ(plan->programs[0].name, "Progra 2");
}

TEST(Tx16wASeries, SynthesizesAuditionProgramsWhenPerformanceFileIsAbsent) {
    axk::tx16w::Inspection inspection;
    inspection.waves.push_back({"TONE.W01", 50'000U, 0U, 2U, false, {0, 16}, {}, {}, {}, {}, {}, {}});
    axk::tx16w::NativeSetup setup;
    setup.name = "VOICEONLY";
    setup.waves.push_back({0U, "TONE"});
    setup.timbres.push_back({0U, "Member", 0U, 76U});
    setup.voices.push_back({2U, "Solo Voice", {{0U, 36U, 96U, 0U}}});
    inspection.setups.push_back(std::move(setup));

    const auto plan = axk::tx16w::a_series::plan_import(inspection);

    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_EQ(plan->programs.size(), 1U);
    EXPECT_EQ(plan->programs[0].number, 1U);
    EXPECT_EQ(plan->programs[0].name, "Solo Voi");
    ASSERT_EQ(plan->programs[0].assignments.size(), 1U);
    EXPECT_EQ(plan->programs[0].assignments[0].target_name, "Solo Voice");
    EXPECT_EQ(plan->programs[0].assignments[0].receive_mode, axk::ProgramReceiveMode::sample);
    EXPECT_TRUE(std::ranges::any_of(plan->notices, [](const auto &notice) {
        return notice.disposition == axk::tx16w::a_series::MappingDisposition::defaulted &&
               notice.source_parameter == "missing_performance";
    }));
}
