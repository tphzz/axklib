#include "validation_plan.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <limits>
#include <ranges>

namespace axk::server::detail {
namespace {

constexpr bool has_type(const SchemaNode &schema, JsonType type) {
    return (schema.types & static_cast<std::uint8_t>(type)) != 0U;
}

const SchemaNode &resolved(const ValidationPlan &plan, const SchemaNode &schema) {
    const SchemaNode *current = &schema;
    for (std::size_t depth = 0U; depth < 64U && current->reference != no_schema; ++depth)
        current = &plan.nodes[current->reference];
    return *current;
}

bool ascii_range(std::string_view value, unsigned char low, unsigned char high) {
    return std::ranges::all_of(value, [=](unsigned char character) { return character >= low && character <= high; });
}

bool digits_01_16(std::string_view value) {
    return value.size() == 2U && ((value[0] == '0' && value[1] >= '1' && value[1] <= '9') ||
                                  (value[0] == '1' && value[1] >= '0' && value[1] <= '6'));
}

std::size_t utf8_length(std::string_view value) {
    return static_cast<std::size_t>(
        std::ranges::count_if(value, [](unsigned char character) { return (character & 0xc0U) != 0x80U; }));
}

bool matches_pattern(Pattern pattern, std::string_view value) {
    switch (pattern) {
    case Pattern::none:
        return true;
    case Pattern::lowercase_sha256:
        return value.size() == 64U && std::ranges::all_of(value, [](unsigned char character) {
                   return std::isdigit(character) != 0 || (character >= 'a' && character <= 'f');
               });
    case Pattern::sampler_name:
        return (value.size() == 1U && ascii_range(value, '!', '~')) ||
               (value.size() >= 2U && value.size() <= 8U &&
                static_cast<unsigned char>(value.front()) >= static_cast<unsigned char>('!') &&
                static_cast<unsigned char>(value.front()) <= static_cast<unsigned char>('~') &&
                static_cast<unsigned char>(value.back()) >= static_cast<unsigned char>('!') &&
                static_cast<unsigned char>(value.back()) <= static_cast<unsigned char>('~') &&
                ascii_range(value, ' ', '~'));
    case Pattern::printable_text:
        return !value.empty() && ascii_range(value, ' ', '~');
    case Pattern::media_extension:
        return value == ".iso" || value == ".ima" || value == ".zip";
    case Pattern::package_extension:
        return value.size() > 4U && value.starts_with(".axk") &&
               std::ranges::all_of(value.substr(4U),
                                   [](unsigned char character) { return std::islower(character) != 0; });
    case Pattern::identifier:
        return !value.empty() && std::ranges::all_of(value, [](unsigned char character) {
            return std::isalnum(character) != 0 || character == '_' || character == '.' || character == '-';
        });
    case Pattern::midi_manufacturer_id: {
        const auto hexadecimal = [](unsigned char character) {
            return std::isdigit(character) != 0 || (character >= 'A' && character <= 'F');
        };
        return (value.size() == 2U && std::ranges::all_of(value, hexadecimal)) ||
               (value.size() == 8U && value.starts_with("00-") && value[5] == '-' &&
                hexadecimal(static_cast<unsigned char>(value[3])) &&
                hexadecimal(static_cast<unsigned char>(value[4])) &&
                hexadecimal(static_cast<unsigned char>(value[6])) && hexadecimal(static_cast<unsigned char>(value[7])));
    }
    case Pattern::receive_channel:
        return digits_01_16(value) ||
               (value.size() == 3U && (value.front() == 'A' || value.front() == 'B') && digits_01_16(value.substr(1U)));
    case Pattern::api_path:
        return value.starts_with("/api/v1/");
    case Pattern::archive_content_path: {
        constexpr std::string_view prefix{"/api/v1/download-archives/"};
        constexpr std::string_view suffix{"/content"};
        if (!value.starts_with(prefix) || !value.ends_with(suffix))
            return false;
        const auto identifier = value.substr(prefix.size(), value.size() - prefix.size() - suffix.size());
        return !identifier.empty() &&
               std::ranges::all_of(identifier, [](unsigned char character) { return std::isalnum(character) != 0; });
    }
    }
    return false;
}

bool matches_literal(const Literal &literal, const nlohmann::json &value) {
    switch (literal.kind) {
    case LiteralKind::null:
        return value.is_null();
    case LiteralKind::boolean:
        return value.is_boolean() && value.get<bool>() == literal.boolean;
    case LiteralKind::integer:
        if (value.is_number_unsigned()) {
            return literal.integer >= 0 && value.get<std::uint64_t>() == static_cast<std::uint64_t>(literal.integer);
        }
        return value.is_number_integer() && value.get<std::int64_t>() == literal.integer;
    case LiteralKind::string:
        return value.is_string() && value.get_ref<const std::string &>() == literal.text;
    }
    return false;
}

bool matches_type(const SchemaNode &schema, const nlohmann::json &value) {
    if (schema.types == 0U)
        return true;
    return (has_type(schema, JsonType::null) && value.is_null()) ||
           (has_type(schema, JsonType::boolean) && value.is_boolean()) ||
           (has_type(schema, JsonType::integer) && (value.is_number_integer() || value.is_number_unsigned())) ||
           (has_type(schema, JsonType::number) && value.is_number()) ||
           (has_type(schema, JsonType::string) && value.is_string()) ||
           (has_type(schema, JsonType::array) && value.is_array()) ||
           (has_type(schema, JsonType::object) && value.is_object());
}

bool validate_node(const ValidationPlan &plan, const SchemaNode &input_schema, const nlohmann::json &value,
                   std::size_t depth) {
    if (depth > 128U)
        return false;
    const auto &schema = resolved(plan, input_schema);
    if (!matches_type(schema, value))
        return false;
    if (schema.literals_count != 0U) {
        const auto literals = plan.literals.subspan(schema.literals_begin, schema.literals_count);
        if (std::ranges::none_of(literals, [&](const Literal &literal) { return matches_literal(literal, value); }))
            return false;
    }
    if (schema.variants_count != 0U) {
        const auto variants = plan.variants.subspan(schema.variants_begin, schema.variants_count);
        const auto matches = std::ranges::count_if(variants, [&](std::uint32_t variant) {
            return validate_node(plan, plan.nodes[variant], value, depth + 1U);
        });
        if (matches != 1)
            return false;
    }
    if (value.is_string()) {
        const auto &text = value.get_ref<const std::string &>();
        const auto length = utf8_length(text);
        if ((schema.has_minimum_length && length < schema.minimum_length) ||
            (schema.has_maximum_length && length > schema.maximum_length) || !matches_pattern(schema.pattern, text))
            return false;
    }
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        const auto below_minimum =
            schema.has_minimum && schema.minimum >= 0 && number < static_cast<std::uint64_t>(schema.minimum);
        const auto above_maximum =
            schema.has_maximum && (schema.maximum < 0 || number > static_cast<std::uint64_t>(schema.maximum));
        if (below_minimum || above_maximum)
            return false;
    } else if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        if ((schema.has_minimum && number < schema.minimum) || (schema.has_maximum && number > schema.maximum))
            return false;
    } else if (value.is_number_float()) {
        const auto number = value.get<double>();
        if ((schema.has_minimum && number < static_cast<double>(schema.minimum)) ||
            (schema.has_maximum && number > static_cast<double>(schema.maximum)))
            return false;
    }
    if (value.is_array()) {
        if ((schema.has_minimum_items && value.size() < schema.minimum_items) ||
            (schema.has_maximum_items && value.size() > schema.maximum_items))
            return false;
        if (schema.unique_items) {
            for (auto left = value.begin(); left != value.end(); ++left)
                if (std::find(std::next(left), value.end(), *left) != value.end())
                    return false;
        }
        if (schema.items != no_schema && std::ranges::any_of(value, [&](const auto &item) {
                return !validate_node(plan, plan.nodes[schema.items], item, depth + 1U);
            }))
            return false;
    }
    if (value.is_object()) {
        if (schema.has_maximum_properties && value.size() > schema.maximum_properties)
            return false;
        const auto properties = plan.properties.subspan(schema.properties_begin, schema.properties_count);
        for (const auto &property : properties) {
            const auto found = value.find(property.name);
            if (found == value.end()) {
                if (property.required)
                    return false;
            } else if (property.schema != no_schema &&
                       !validate_node(plan, plan.nodes[property.schema], *found, depth + 1U)) {
                return false;
            }
        }
        for (const auto &[name, child] : value.items()) {
            if (std::ranges::any_of(properties, [&](const Property &property) { return property.name == name; }))
                continue;
            if (!schema.additional_properties_allowed ||
                (schema.additional_properties != no_schema &&
                 !validate_node(plan, plan.nodes[schema.additional_properties], child, depth + 1U)))
                return false;
        }
    }
    return true;
}

nlohmann::json translate_node(const ValidationPlan &plan, const SchemaNode &input_schema, const nlohmann::json &value,
                              bool to_application, std::size_t depth) {
    if (depth > 128U)
        return value;
    const auto &schema = resolved(plan, input_schema);
    if (value.is_string() && schema.mappings_count != 0U) {
        const auto mappings = plan.mappings.subspan(schema.mappings_begin, schema.mappings_count);
        const auto text = value.get_ref<const std::string &>();
        const auto found = std::ranges::find_if(mappings, [&](const EnumMapping &mapping) {
            return (to_application ? mapping.wire : mapping.application) == text;
        });
        return found == mappings.end() ? value
                                       : nlohmann::json(std::string{to_application ? found->application : found->wire});
    }
    auto result = value;
    for (const auto variant : plan.variants.subspan(schema.variants_begin, schema.variants_count))
        result = translate_node(plan, plan.nodes[variant], result, to_application, depth + 1U);
    if (result.is_object()) {
        for (const auto &property : plan.properties.subspan(schema.properties_begin, schema.properties_count))
            if (property.schema != no_schema && result.contains(property.name))
                result[property.name] = translate_node(plan, plan.nodes[property.schema], result.at(property.name),
                                                       to_application, depth + 1U);
    } else if (result.is_array() && schema.items != no_schema) {
        for (auto &item : result)
            item = translate_node(plan, plan.nodes[schema.items], item, to_application, depth + 1U);
    }
    return result;
}

} // namespace

const SchemaNode *find_schema(const ValidationPlan &plan, std::string_view name) {
    const auto found = std::ranges::lower_bound(plan.schemas, name, {}, &NamedSchema::name);
    return found != plan.schemas.end() && found->name == name ? &plan.nodes[found->schema] : nullptr;
}

bool validate(const ValidationPlan &plan, const SchemaNode &schema, const nlohmann::json &value) {
    return validate_node(plan, schema, value, 0U);
}

nlohmann::json translate(const ValidationPlan &plan, const SchemaNode &schema, const nlohmann::json &value,
                         bool to_application) {
    return translate_node(plan, schema, value, to_application, 0U);
}

} // namespace axk::server::detail
