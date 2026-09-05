#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/writer.hpp"
#include "axklib/writer_internal.hpp"

namespace {

constexpr std::array flat{
    std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0x20},
    std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
};

void expect_vector(std::span<const std::byte> vector, const nlohmann::json &reference) {
    ASSERT_EQ(vector.size(), 10U);
    const auto expected = reference.at("coefficients_q13").get<std::array<std::int32_t, 5>>();
    const auto tolerance = reference.at("tolerance_q13").get<std::int32_t>();
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto raw = (std::to_integer<std::int32_t>(vector[index * 2U]) << 8U) |
                         std::to_integer<std::int32_t>(vector[index * 2U + 1U]);
        const auto signed_value = raw >= 32768 ? raw - 65536 : raw;
        EXPECT_LE(std::abs(signed_value - expected[index]), tolerance) << index;
    }
}

} // namespace

TEST(SampleEq, SharedReferencesCoverFreshSamplesBankStateAndExplicitEdits) {
    std::ifstream source{std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/sample-eq-vectors.json"};
    ASSERT_TRUE(source);
    const auto references = nlohmann::json::parse(source);
    ASSERT_FALSE(references.empty());
    for (const auto &reference : references) {
        SCOPED_TRACE(reference.at("name").get<std::string>());
        axk::SampleSpec sample;
        sample.name = "EQ";
        auto &parameters = sample.parameters;
        parameters.sample_eq_type = reference.at("type").get<std::uint8_t>();
        parameters.sample_eq_frequency = reference.at("frequency").get<std::uint8_t>();
        parameters.sample_eq_gain_db = reference.at("gain_db").get<std::int8_t>();
        parameters.sample_eq_width_tenths = reference.at("width_tenths").get<std::uint8_t>();
        const auto fresh = axk::detail::prepare_sbnk_payload(sample, {"Wave", 0x100U, 44'100U, 400U});
        ASSERT_TRUE(fresh);
        expect_vector(std::span{*fresh}.subspan(0x152U, 10U), reference);

        axk::SampleBankSpec bank;
        bank.name = "EQ Bank";
        bank.member_samples = {"EQ"};
        bank.parameter_overrides = parameters;
        const auto bank_payload = axk::detail::prepare_sbac_payload(bank, {{sample.name, sample}}, {1U});
        ASSERT_TRUE(bank_payload);
        expect_vector(std::span{*bank_payload}.subspan(0x122U, 10U), reference);

        std::array<axk::SampleParameters, 4> edits;
        edits[0].sample_eq_type = parameters.sample_eq_type;
        edits[1].sample_eq_frequency = parameters.sample_eq_frequency;
        edits[2].sample_eq_gain_db = parameters.sample_eq_gain_db;
        edits[3].sample_eq_width_tenths = parameters.sample_eq_width_tenths;
        for (const auto &edit : edits) {
            auto payload = *fresh;
            std::ranges::copy(flat, payload.begin() + 0x152U);
            axk::SampleParameters unrelated;
            unrelated.level = 99U;
            ASSERT_TRUE(axk::detail::apply_sample_parameters_to_payload(payload, unrelated));
            EXPECT_TRUE(std::ranges::equal(flat, std::span{payload}.subspan(0x152U, 10U)));
            ASSERT_TRUE(axk::detail::apply_sample_parameters_to_payload(payload, edit));
            expect_vector(std::span{payload}.subspan(0x152U, 10U), reference);
        }
    }
}
