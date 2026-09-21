#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/sample_bank_overrides.hpp"
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
        EXPECT_EQ(bank.override_enable_words[1], 0xe0002U);
        EXPECT_EQ(bank.override_enable_words[2], format == Format::a3000_188 ? 0U : 1U << 21U);
        const auto values = bank.raw_sample_parameter_block;
        ASSERT_TRUE(axk::apply_sample_bank_overrides(payload, {{}, {}, {49}}));
        EXPECT_EQ(decode(payload).raw_sample_parameter_block, values);
        EXPECT_EQ(decode(payload).override_enable_words[1], 2U);
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
