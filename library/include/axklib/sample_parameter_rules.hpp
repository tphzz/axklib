#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/sample_parameters.hpp"

namespace axk {

enum class SampleParameterEncoding : std::uint8_t { u8, s8, be16, be32, bits };

struct SampleParameterLocation {
    std::size_t offset{};
    SampleParameterEncoding encoding{SampleParameterEncoding::u8};
    std::int64_t minimum{};
    std::int64_t maximum{};
    std::uint8_t mask{255U};
    std::uint8_t shift{};
    int bias{};
    std::optional<std::int64_t> extra_value;
    std::optional<std::int64_t> a5000_minimum;
};

struct SampleParameterRule {
    std::string_view key;
    std::optional<SampleParameterLocation> a3000;
    std::optional<SampleParameterLocation> a4000_a5000;
    std::optional<std::int64_t> (*get)(const SampleParameters &);
    void (*set)(SampleParameters &, std::int64_t);
    bool boolean{};
};

struct SampleParameterIssue {
    std::string key;
    std::string message;
    std::optional<std::int64_t> stored_value;
};

AXK_API std::span<const SampleParameterRule> sample_parameter_rules();
AXK_API std::optional<SampleParameterLocation> sample_parameter_location(const SampleParameterRule &rule,
                                                                         SampleParameterGeneration generation);
AXK_API bool sample_parameter_value_allowed(const SampleParameterLocation &location, std::int64_t value);
AXK_API std::optional<std::int64_t> read_sample_parameter_value(std::span<const std::byte> block,
                                                                const SampleParameterLocation &location);
AXK_API Result<void> write_sample_parameter_value(std::span<std::byte> block, const SampleParameterLocation &location,
                                                  std::int64_t value);
AXK_API std::vector<SampleParameterIssue> assess_sample_parameter_block(std::span<const std::byte> block,
                                                                        SampleParameterGeneration generation);

} // namespace axk
