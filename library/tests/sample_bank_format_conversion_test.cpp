#include <algorithm>
#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../src/alteration_internal.hpp"
#include "axklib/bytes.hpp"
#include "axklib/object.hpp"
#include "axklib/sample_format_conversion.hpp"
#include "axklib/writer.hpp"
#include "axklib/writer_internal.hpp"

namespace {
using Format = axk::SampleStorageFormat;

std::vector<std::byte> bank_payload(Format format, unsigned count = 1) {
    axk::SampleBankSpec bank;
    bank.name = "Bank";
    bank.storage_format = format;
    std::map<std::string, axk::SampleSpec> samples;
    for (unsigned i = 0; i < count; ++i) {
        axk::SampleSpec sample;
        sample.name = "Member" + std::to_string(i);
        bank.member_samples.push_back(sample.name);
        samples.emplace(sample.name, sample);
    }
    const auto result = axk::detail::prepare_sbac_payload(bank, samples);
    EXPECT_TRUE(result);
    return result ? *result : std::vector<std::byte>{};
}

TEST(SampleBankFormat, RecognizesStoredLayoutAndIgnoresAllocationPadding) {
    for (const auto format : {Format::a3000_188, Format::a4000_a5000_224}) {
        for (const auto count : {1U, 8U, 9U, 127U}) {
            auto bytes = bank_payload(format, count);
            bytes.resize(bytes.size() + 512, std::byte{0xad});
            const auto storage = axk::inspect_sample_bank_storage(bytes);
            ASSERT_TRUE(storage.structurally_valid);
            EXPECT_EQ(storage.format, format);
            EXPECT_EQ(storage.parameter_bytes, format == Format::a3000_188 ? 188U : 224U);
            const auto blocks = axk::read_sample_bank_parameter_blocks(bytes);
            ASSERT_TRUE(blocks);
            EXPECT_EQ(blocks->extension.has_value(), format == Format::a4000_a5000_224);
        }
    }
}

TEST(SampleBankFormat, NativeRoundTripPreservesRowsCapacityAndPadding) {
    auto bytes = bank_payload(Format::a3000_188, 9);
    bytes[0x90] = std::byte{0x43};
    bytes[0x143] = std::byte{0x25};
    bytes[0x15c] = std::byte{0x76};
    bytes.resize(bytes.size() + 97, std::byte{0xad});
    const auto up = axk::plan_sample_bank_format_conversion(bytes, Format::a4000_a5000_224);
    ASSERT_TRUE(up.allowed()) << (up.blockers.empty() ? "" : up.blockers.front().message);
    EXPECT_EQ(up.converted_payload.size(), bytes.size() + 36);
    const auto down = axk::plan_sample_bank_format_conversion(up.converted_payload, Format::a3000_188);
    ASSERT_TRUE(down.allowed()) << (down.blockers.empty() ? "" : down.blockers.front().message);
    EXPECT_EQ(down.converted_payload, bytes);
    const auto same = axk::plan_sample_bank_format_conversion(bytes, Format::a3000_188);
    EXPECT_TRUE(same.no_op);
    EXPECT_EQ(same.converted_payload, bytes);
}

TEST(SampleBankFormat, RejectsUnknownHeadersTruncationAndPartialRows) {
    const auto bytes = bank_payload(Format::a3000_188);
    for (const auto kind : {0U, 0x10U, 0x14U, 0xffU}) {
        auto changed = bytes;
        changed[0x30] = static_cast<std::byte>(kind);
        EXPECT_FALSE(axk::inspect_sample_bank_storage(changed).structurally_valid);
    }
    for (const auto revision : {0U, 1U, 3U, 5U}) {
        auto changed = bytes;
        ASSERT_TRUE(axk::ByteWriter{changed}.write_be32(0x14, revision));
        EXPECT_FALSE(axk::inspect_sample_bank_storage(changed).structurally_valid);
        EXPECT_FALSE(axk::plan_sample_bank_format_conversion(changed, Format::a4000_a5000_224).allowed());
    }
    auto changed = bytes;
    changed.pop_back();
    EXPECT_FALSE(axk::inspect_sample_bank_storage(changed).structurally_valid);
    ASSERT_TRUE(axk::ByteWriter{changed}.write_be32(0x18, static_cast<unsigned>(changed.size() - 0x30)));
    EXPECT_FALSE(axk::inspect_sample_bank_storage(changed).structurally_valid);
    changed = bytes;
    changed[0x144] = std::byte{127};
    EXPECT_FALSE(axk::inspect_sample_bank_storage(changed).structurally_valid);
}

TEST(SampleBankFormat, PaddedConvertedBankCanBeEditedAndGrown) {
    for (const auto target : {Format::a3000_188, Format::a4000_a5000_224}) {
        const auto native = bank_payload(Format::a3000_188, 8);
        auto initial = axk::plan_sample_bank_format_conversion(native, Format::a4000_a5000_224).converted_payload;
        initial.resize(initial.size() + 97, std::byte{0xad});
        auto plan = axk::plan_sample_bank_format_conversion(initial, target);
        ASSERT_TRUE(plan.allowed());
        auto &bytes = plan.converted_payload;
        axk::SampleParameters edit;
        edit.level = 82;
        ASSERT_TRUE(axk::detail::apply_sample_bank_parameters_to_payload(bytes, edit));
        EXPECT_EQ(bytes[0xe6], std::byte{82});
        const auto decoded = axk::decode_object(bytes);
        ASSERT_TRUE(decoded);
        ASSERT_TRUE(axk::alteration_internal::append_sbac_members_to_payload(
            bytes, std::get<axk::CurrentSbac>(decoded->payload), {"Ninth"}));
        const auto grown = axk::decode_object(bytes);
        ASSERT_TRUE(grown);
        EXPECT_EQ(std::get<axk::CurrentSbac>(grown->payload).maximum_member_count, 9U);
        EXPECT_TRUE(std::ranges::all_of(std::span{bytes}.last(97), [](auto b) { return b == std::byte{0xad}; }));
        EXPECT_TRUE(axk::inspect_sample_bank_storage(bytes).structurally_valid);
    }
}

TEST(SampleBankFormat, BlocksUnrepresentableParametersAndUnknownPendingBits) {
    auto bytes = bank_payload(Format::a3000_188);
    bytes[0xa5] = std::byte{100};
    EXPECT_FALSE(axk::plan_sample_bank_format_conversion(bytes, Format::a4000_a5000_224).allowed());
    bytes = bank_payload(Format::a3000_188);
    bytes[0x13c] = std::byte{0x80};
    EXPECT_FALSE(axk::plan_sample_bank_format_conversion(bytes, Format::a4000_a5000_224).allowed());
    bytes = bank_payload(Format::a4000_a5000_224);
    bytes[bytes.size() - 36 + 24] = std::byte{3};
    EXPECT_FALSE(axk::plan_sample_bank_format_conversion(bytes, Format::a3000_188).allowed());
}

TEST(SampleBankFormat, PendingOperationRangeDependsOnTheStoredGeneration) {
    for (const auto format : {Format::a3000_188, Format::a4000_a5000_224}) {
        auto bytes = bank_payload(format);
        ASSERT_TRUE(axk::ByteWriter{bytes}.write_be32(0x13c, 0x01f00000U));
        const auto decoded = axk::decode_object(bytes);
        ASSERT_TRUE(decoded);
        const auto &bank = std::get<axk::CurrentSbac>(decoded->payload);
        EXPECT_EQ(bank.override_selectors.size(), format == Format::a3000_188 ? 1U : 5U);
        EXPECT_EQ(bank.reserved_override_selectors.size(), format == Format::a3000_188 ? 4U : 0U);
        EXPECT_FALSE(axk::plan_sample_bank_format_conversion(
                         bytes, format == Format::a3000_188 ? Format::a4000_a5000_224 : Format::a3000_188)
                         .allowed());
    }
}
} // namespace
