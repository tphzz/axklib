#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include <nlohmann/json.hpp>

namespace axk::server::detail {

constexpr std::uint32_t no_schema = std::numeric_limits<std::uint32_t>::max();

enum class JsonType : std::uint8_t {
    null = 1U << 0U,
    boolean = 1U << 1U,
    integer = 1U << 2U,
    number = 1U << 3U,
    string = 1U << 4U,
    array = 1U << 5U,
    object = 1U << 6U,
};

enum class Pattern : std::uint8_t {
    none,
    lowercase_sha256,
    sampler_name,
    printable_text,
    media_extension,
    package_extension,
    identifier,
    midi_manufacturer_id,
    receive_channel,
    api_path,
    archive_content_path,
};

enum class LiteralKind : std::uint8_t { null, boolean, integer, string };

struct Literal {
    LiteralKind kind;
    std::string_view text;
    std::int64_t integer;
    bool boolean;
};

struct Property {
    std::string_view name;
    std::uint32_t schema;
    bool required;
};

struct EnumMapping {
    std::string_view wire;
    std::string_view application;
};

struct SchemaNode {
    std::uint8_t types;
    Pattern pattern;
    std::uint32_t reference;
    std::uint32_t items;
    std::uint32_t additional_properties;
    std::uint32_t properties_begin;
    std::uint32_t properties_count;
    std::uint32_t variants_begin;
    std::uint32_t variants_count;
    std::uint32_t literals_begin;
    std::uint32_t literals_count;
    std::uint32_t mappings_begin;
    std::uint32_t mappings_count;
    std::uint64_t minimum_length;
    std::uint64_t maximum_length;
    std::uint64_t minimum_items;
    std::uint64_t maximum_items;
    std::uint64_t maximum_properties;
    std::int64_t minimum;
    std::int64_t maximum;
    bool additional_properties_allowed;
    bool unique_items;
    bool has_minimum_length;
    bool has_maximum_length;
    bool has_minimum_items;
    bool has_maximum_items;
    bool has_maximum_properties;
    bool has_minimum;
    bool has_maximum;
};

struct NamedSchema {
    std::string_view name;
    std::uint32_t schema;
};

struct ValidationPlan {
    std::span<const SchemaNode> nodes;
    std::span<const Property> properties;
    std::span<const std::uint32_t> variants;
    std::span<const Literal> literals;
    std::span<const EnumMapping> mappings;
    std::span<const NamedSchema> schemas;
};

[[nodiscard]] const ValidationPlan &generated_validation_plan();
[[nodiscard]] const SchemaNode *find_schema(const ValidationPlan &plan, std::string_view name);
[[nodiscard]] bool validate(const ValidationPlan &plan, const SchemaNode &schema, const nlohmann::json &value);
[[nodiscard]] nlohmann::json translate(const ValidationPlan &plan, const SchemaNode &schema,
                                       const nlohmann::json &value, bool to_application);

} // namespace axk::server::detail
