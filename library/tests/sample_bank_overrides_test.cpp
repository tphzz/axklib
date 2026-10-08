#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/sample_bank_overrides.hpp"
#include "axklib/sample_format_conversion.hpp"
#include "axklib/writer_internal.hpp"

namespace {
using Format = axk::SampleStorageFormat;
std::vector<std::byte> empty_bank(Format format) {
    axk::SampleBankSpec bank;
    bank.name = "Empty";
    bank.storage_format = format;
    bank.member_samples = {"Member"};
    axk::SampleSpec sample;
    sample.name = "Member";
    auto result = axk::detail::prepare_sbac_payload(bank, {{"Member", sample}});
    if (!result)
        throw std::runtime_error(result.error().message);
    (*result)[0x144] = std::byte{0};
    std::fill_n(result->begin() + 0x14c, 20, std::byte{0});
    return *result;
}
axk::CurrentSbac decode(const std::vector<std::byte> &payload) {
    const auto result = axk::decode_object(payload);
    EXPECT_TRUE(result);
    return std::get<axk::CurrentSbac>(result->payload);
}

TEST(SampleBankOverrides, EmptyBanksSupportFlagOnlyActivationWithoutChangingDormantValues) {
    for (const auto format : {Format::a3000_188, Format::a4000_a5000_224}) {
        auto payload = empty_bank(format);
        payload[0x6c] = std::byte{0x71};
        payload[0x6d] = std::byte{0x72};
        payload[0x6e] = std::byte{0x73};
        payload[0x143] = std::byte{0xff};
        const auto original = payload;
        auto expected = original;
        ASSERT_TRUE(axk::ByteWriter{expected}.write_be32(0x138, 0xe0000U));
        ASSERT_TRUE(axk::ByteWriter{expected}.write_be32(0x13c, format == Format::a3000_188 ? 0x80000U : 0x280000U));
        const auto enabled = axk::apply_sample_bank_overrides(payload, {{}, {49, 83}, {}});
        ASSERT_TRUE(enabled) << enabled.error().message;
        EXPECT_EQ(payload, expected);
        ASSERT_TRUE(axk::apply_sample_bank_overrides(payload, {{}, {}, {49, 83}}));
        EXPECT_EQ(payload, original);
    }
}

TEST(SampleBankOverrides, PartialEqMasksSurviveUnrelatedEditsAndNormalizeOnEqValueEdit) {
    for (const auto format : {Format::a3000_188, Format::a4000_a5000_224}) {
        auto payload = empty_bank(format);
        ASSERT_TRUE(axk::ByteWriter{payload}.write_be32(0x134, 1U << 6U));
        payload[0xa6] = std::byte{67};
        payload[0xae] = std::byte{0x12};
        payload[0xaf] = std::byte{0xab};
        const auto original = decode(payload).raw_sample_parameter_block;
        ASSERT_TRUE(axk::ByteWriter{payload}.write_be32(0x138, 1U << 18U));
        axk::SampleBankOverrideEdit edit;
        edit.enable = {33};
        edit.parameters.level = 82;
        ASSERT_TRUE(axk::apply_sample_bank_overrides(payload, edit));
        EXPECT_EQ(decode(payload).override_enable_words[1], (1U << 18U) | 2U);
        edit = {};
        edit.parameters.sample_eq_gain_db = 4;
        ASSERT_TRUE(axk::apply_sample_bank_overrides(payload, edit));
        const auto bank = decode(payload);
        for (std::size_t offset = 0x2eU; offset < 0x3aU; ++offset)
            EXPECT_EQ(bank.raw_sample_parameter_block[offset], original[offset]);
        EXPECT_EQ(bank.override_enable_words[1], 0xe0002U);
        EXPECT_EQ(bank.override_enable_words[2], format == Format::a3000_188 ? 0U : 1U << 21U);
        const auto values = bank.raw_sample_parameter_block;
        ASSERT_TRUE(axk::apply_sample_bank_overrides(payload, {{}, {}, {49}}));
        EXPECT_EQ(decode(payload).raw_sample_parameter_block, values);
        EXPECT_EQ(decode(payload).override_enable_words[1], 2U);
        EXPECT_EQ(decode(payload).override_enable_words[0], 1U << 6U);
    }
}

TEST(SampleBankOverrides, PreservedRootSelectorAdmitsOnlySupportedEditsAndNeverRewritesDormantPitchBytes) {
    for (const auto format : {Format::a3000_188, Format::a4000_a5000_224}) {
        SCOPED_TRACE(axk::sample_storage_format_name(format));
        auto payload = empty_bank(format);
        constexpr std::array pitch{std::byte{67},   std::byte{60},   std::byte{0x56}, std::byte{0x22},
                                   std::byte{0xac}, std::byte{0x44}, std::byte{0xff}, std::byte{0},
                                   std::byte{0x12}, std::byte{0xab}, std::byte{0x34}, std::byte{0xcd}};
        std::ranges::copy(pitch, payload.begin() + 0xa6);
        ASSERT_TRUE(axk::ByteWriter{payload}.write_be32(0x134, 1U << 6U));
        ASSERT_TRUE(axk::ByteWriter{payload}.write_be32(0x140, 0xabcdef01U));
        payload[0x14c] = std::byte{0x75};
        const auto bank = decode(payload);
        ASSERT_TRUE(axk::sample_bank_override_state_supported(bank));
        for (const auto &unit : axk::sample_bank_override_units(*axk::sample_parameter_generation(format))) {
            EXPECT_NE(unit.id, 6U);
            EXPECT_FALSE(std::ranges::contains(unit.selectors, 6U));
            EXPECT_FALSE(std::ranges::contains(unit.keys, "root_key"));
        }
        auto expected = payload;
        expected[0xe6] = std::byte{81};
        ASSERT_TRUE(axk::ByteWriter{expected}.write_be32(0x138, 2U));
        axk::SampleBankOverrideEdit edit;
        edit.enable = {33};
        edit.parameters.level = 81;
        ASSERT_TRUE(axk::apply_sample_bank_overrides(payload, edit));
        EXPECT_EQ(payload, expected);
        edit = {};
        edit.enable = {65};
        edit.parameters.aeg.attack_rate = 100;
        expected[0x10c] = std::byte{100};
        ASSERT_TRUE(axk::ByteWriter{expected}.write_be32(0x13c, 2U));
        ASSERT_TRUE(axk::apply_sample_bank_overrides(payload, edit));
        EXPECT_EQ(payload, expected);
        ASSERT_TRUE(axk::apply_sample_bank_overrides(payload, {{}, {}, {33, 65}}));
        ASSERT_TRUE(axk::ByteWriter{expected}.write_be32(0x138, 0U));
        ASSERT_TRUE(axk::ByteWriter{expected}.write_be32(0x13c, 0U));
        EXPECT_EQ(payload, expected);
        const auto other = format == Format::a3000_188 ? Format::a4000_a5000_224 : Format::a3000_188;
        EXPECT_FALSE(axk::plan_sample_bank_format_conversion(payload, other).allowed());
    }
}

TEST(SampleBankOverrides, RootSelectorCannotBeEnabledDisabledOrEditedEvenAlongsideValidChanges) {
    for (const auto format : {Format::a3000_188, Format::a4000_a5000_224}) {
        for (const bool root_enabled : {false, true}) {
            auto payload = empty_bank(format);
            ASSERT_TRUE(axk::ByteWriter{payload}.write_be32(0x134, root_enabled ? 1U << 6U : 0U));
            const auto original = payload;
            axk::SampleBankOverrideEdit valid;
            valid.enable = {33};
            valid.parameters.level = 81;
            auto root = valid;
            root.parameters.root_key = 64;
            auto enable = valid;
            enable.enable.push_back(6);
            auto disable = valid;
            disable.disable = {6};
            for (const auto &edit : {root, enable, disable}) {
                EXPECT_FALSE(axk::apply_sample_bank_overrides(payload, edit));
                EXPECT_EQ(payload, original);
            }
        }
        for (const auto selector : {0U, 7U, 86U, 95U}) {
            auto payload = empty_bank(format);
            ASSERT_TRUE(axk::ByteWriter{payload}.write_be32(0x134, 1U << 6U));
            const auto offset = 0x134U + 4U * (selector / 32U);
            const auto flags = (selector < 32U ? 1U << 6U : 0U) | (1U << (selector % 32U));
            ASSERT_TRUE(axk::ByteWriter{payload}.write_be32(offset, flags));
            const auto original = payload;
            EXPECT_FALSE(axk::sample_bank_override_state_supported(decode(payload)));
            EXPECT_FALSE(axk::apply_sample_bank_overrides(payload, {{}, {33}, {}}));
            EXPECT_EQ(payload, original);
        }
        auto bank = decode(empty_bank(format));
        bank.override_enable_words[0] = 1U << 6U;
        bank.storage.structurally_valid = false;
        EXPECT_FALSE(axk::sample_bank_override_state_supported(bank));
    }
}

TEST(SampleBankOverrides, InvalidGroupsUnsupportedStateAndNonBankFieldsNeverMutatePayload) {
    for (const auto format : {Format::a3000_188, Format::a4000_a5000_224}) {
        for (const auto offset : {0x64U, 0x74U}) {
            auto payload = empty_bank(format);
            payload[0x78 + offset] = std::byte{100};
            payload[0x79 + offset] = std::byte{20};
            const auto original = payload;
            EXPECT_FALSE(axk::apply_sample_bank_overrides(
                payload, {{}, {static_cast<std::uint8_t>(offset == 0x64U ? 24 : 39)}, {}}));
            EXPECT_EQ(payload, original);
        }
        for (const auto selector : {0U, 86U, 95U}) {
            auto payload = empty_bank(format);
            ASSERT_TRUE(axk::ByteWriter{payload}.write_be32(0x134 + 4 * (selector / 32), 1U << (selector % 32)));
            const auto original = payload;
            EXPECT_FALSE(axk::apply_sample_bank_overrides(payload, {{}, {33}, {}}));
            EXPECT_EQ(payload, original);
        }
        auto payload = empty_bank(format);
        const auto original = payload;
        axk::SampleBankOverrideEdit edit;
        edit.parameters.root_key = 60;
        EXPECT_FALSE(axk::apply_sample_bank_overrides(payload, edit));
        edit.parameters = {};
        edit.parameters.level = 80;
        EXPECT_FALSE(axk::apply_sample_bank_overrides(payload, edit));
        edit.enable = {33};
        edit.disable = {33};
        EXPECT_FALSE(axk::apply_sample_bank_overrides(payload, edit));
        EXPECT_EQ(payload, original);
    }
}

TEST(SampleBankOverrides, CutoffDistanceUsesBankTypeOnlyWhenTypeIsOverridden) {
    auto payload = empty_bank(Format::a4000_a5000_224);
    payload[0xd9] = std::byte{1};
    ASSERT_TRUE(axk::apply_sample_bank_overrides(payload, {{}, {52}, {}}));
    ASSERT_TRUE(axk::apply_sample_bank_overrides(payload, {{}, {21}, {52}}));
    const auto original = payload;
    EXPECT_FALSE(axk::apply_sample_bank_overrides(payload, {{}, {52}, {}}));
    EXPECT_EQ(payload, original);
    axk::SampleBankOverrideEdit edit;
    edit.enable = {52};
    edit.parameters.filter_type = 10;
    ASSERT_TRUE(axk::apply_sample_bank_overrides(payload, edit));
}
} // namespace
