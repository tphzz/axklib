#include <algorithm>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "validation_plan.hpp"

namespace {

using Json = nlohmann::json;
using axk::server::detail::EnumMapping;
using axk::server::detail::Literal;
using axk::server::detail::LiteralKind;
using axk::server::detail::NamedSchema;
using axk::server::detail::Pattern;
using axk::server::detail::Property;
using axk::server::detail::SchemaNode;
constexpr auto no_schema = axk::server::detail::no_schema;
constexpr std::string_view reference_prefix{"#/components/schemas/"};

std::string quote(std::string_view value) { return Json(std::string{value}).dump(); }

std::string pattern_name(std::string_view value) {
    static const std::map<std::string_view, std::string_view> patterns{
        {"", "none"},
        {"^[0-9a-f]{64}$", "lowercase_sha256"},
        {"^(?:[!-~]|[!-~][ -~]{0,6}[!-~])$", "sampler_name"},
        {"^[ -~]+$", "printable_text"},
        {"^\\.(iso|ima|zip)$", "media_extension"},
        {"^\\.axk[a-z]+$", "package_extension"},
        {"^[A-Za-z0-9_.-]+$", "identifier"},
        {"^(?:[0-9A-F]{2}|00-[0-9A-F]{2}-[0-9A-F]{2})$", "midi_manufacturer_id"},
        {"^(A|B)?(0[1-9]|1[0-6])$", "receive_channel"},
        {"^/api/v1/", "api_path"},
        {"^/api/v1/download-archives/[A-Za-z0-9]+/content$", "archive_content_path"},
    };
    const auto found = patterns.find(value);
    if (found == patterns.end())
        throw std::runtime_error{"unsupported OpenAPI pattern: " + std::string{value}};
    return "Pattern::" + std::string{found->second};
}

std::uint8_t type_bit(std::string_view type) {
    static const std::map<std::string_view, std::uint8_t> types{
        {"null", 1U << 0U},   {"boolean", 1U << 1U}, {"integer", 1U << 2U}, {"number", 1U << 3U},
        {"string", 1U << 4U}, {"array", 1U << 5U},   {"object", 1U << 6U},
    };
    const auto found = types.find(type);
    if (found == types.end())
        throw std::runtime_error{"unsupported OpenAPI type: " + std::string{type}};
    return found->second;
}

struct Compiler {
    explicit Compiler(const Json &document) : document(document) {
        for (const auto &[name, schema] : document.at("components").at("schemas").items()) {
            static_cast<void>(schema);
            names.emplace(name, static_cast<std::uint32_t>(nodes.size()));
            named.push_back({intern(name), static_cast<std::uint32_t>(nodes.size())});
            nodes.emplace_back();
        }
        for (const auto &[name, schema] : document.at("components").at("schemas").items())
            compile_at(names.at(name), schema, name);
        std::ranges::sort(named, {}, &NamedSchema::name);
    }

    std::uint32_t compile(const Json &schema, std::string_view path) {
        const auto index = static_cast<std::uint32_t>(nodes.size());
        nodes.emplace_back();
        compile_at(index, schema, path);
        return index;
    }

    void compile_at(std::uint32_t index, const Json &schema, std::string_view path) {
        if (!schema.is_object())
            throw std::runtime_error{"schema is not an object at " + std::string{path}};
        static const std::set<std::string_view> supported{
            "$ref",
            "type",
            "properties",
            "required",
            "additionalProperties",
            "items",
            "enum",
            "const",
            "minimum",
            "maximum",
            "minLength",
            "maxLength",
            "minItems",
            "maxItems",
            "maxProperties",
            "uniqueItems",
            "oneOf",
            "pattern",
            "default",
            "description",
            "example",
            "x-axklib-application-enum",
        };
        for (const auto &[keyword, value] : schema.items()) {
            static_cast<void>(value);
            if (!supported.contains(keyword))
                throw std::runtime_error{"unsupported OpenAPI schema keyword at " + std::string{path} + ": " + keyword};
        }

        SchemaNode node{};
        node.reference = no_schema;
        node.items = no_schema;
        node.additional_properties = no_schema;
        node.additional_properties_allowed = true;
        if (const auto reference = schema.find("$ref"); reference != schema.end()) {
            if (!reference->is_string() || !reference->get_ref<const std::string &>().starts_with(reference_prefix))
                throw std::runtime_error{"unsupported OpenAPI reference at " + std::string{path}};
            node.reference = compile_reference(reference->get_ref<const std::string &>());
            nodes[index] = node;
            return;
        }
        if (const auto type = schema.find("type"); type != schema.end()) {
            if (type->is_string()) {
                node.types = type_bit(type->get_ref<const std::string &>());
            } else if (type->is_array()) {
                for (const auto &candidate : *type) {
                    if (!candidate.is_string())
                        throw std::runtime_error{"non-string OpenAPI type at " + std::string{path}};
                    node.types |= type_bit(candidate.get_ref<const std::string &>());
                }
            } else {
                throw std::runtime_error{"invalid OpenAPI type at " + std::string{path}};
            }
        }
        node.pattern = pattern_from(schema.value("pattern", std::string{}));
        if (const auto items = schema.find("items"); items != schema.end())
            node.items = compile(*items, std::string{path} + "[]");

        std::set<std::string> required;
        if (const auto values = schema.find("required"); values != schema.end()) {
            if (!values->is_array())
                throw std::runtime_error{"required is not an array at " + std::string{path}};
            for (const auto &value : *values)
                required.insert(value.get<std::string>());
        }
        std::vector<Property> local_properties;
        if (const auto values = schema.find("properties"); values != schema.end()) {
            if (!values->is_object())
                throw std::runtime_error{"properties is not an object at " + std::string{path}};
            for (const auto &[name, child] : values->items())
                local_properties.push_back(
                    {intern(name), compile(child, std::string{path} + '/' + name), required.contains(name)});
        }
        for (const auto &name : required)
            if (std::ranges::none_of(local_properties, [&](const Property &property) { return property.name == name; }))
                local_properties.push_back({intern(name), no_schema, true});
        node.properties_begin = append(properties, local_properties);
        node.properties_count = static_cast<std::uint32_t>(local_properties.size());

        if (const auto additional = schema.find("additionalProperties"); additional != schema.end()) {
            if (additional->is_boolean()) {
                node.additional_properties_allowed = additional->get<bool>();
            } else if (additional->is_object()) {
                node.additional_properties = compile(*additional, std::string{path} + "/additionalProperties");
            } else {
                throw std::runtime_error{"invalid additionalProperties at " + std::string{path}};
            }
        }
        std::vector<std::uint32_t> local_variants;
        if (const auto values = schema.find("oneOf"); values != schema.end()) {
            if (!values->is_array())
                throw std::runtime_error{"oneOf is not an array at " + std::string{path}};
            for (std::size_t variant = 0U; variant < values->size(); ++variant)
                local_variants.push_back(compile(values->at(variant), std::string{path} + "/oneOf"));
        }
        node.variants_begin = append(variants, local_variants);
        node.variants_count = static_cast<std::uint32_t>(local_variants.size());

        std::vector<Literal> local_literals;
        if (const auto enumeration = schema.find("enum"); enumeration != schema.end()) {
            if (!enumeration->is_array())
                throw std::runtime_error{"enum is not an array at " + std::string{path}};
            for (const auto &value : *enumeration)
                local_literals.push_back(literal(value, path));
        }
        if (const auto constant = schema.find("const"); constant != schema.end())
            local_literals.push_back(literal(*constant, path));
        node.literals_begin = append(literals, local_literals);
        node.literals_count = static_cast<std::uint32_t>(local_literals.size());

        std::vector<EnumMapping> local_mappings;
        if (const auto mapping = schema.find("x-axklib-application-enum"); mapping != schema.end()) {
            if (!mapping->is_object())
                throw std::runtime_error{"application enum mapping is not an object at " + std::string{path}};
            for (const auto &[wire, application] : mapping->items()) {
                if (!application.is_string())
                    throw std::runtime_error{"application enum value is not a string at " + std::string{path}};
                local_mappings.push_back({intern(wire), intern(application.get_ref<const std::string &>())});
            }
        }
        node.mappings_begin = append(mappings, local_mappings);
        node.mappings_count = static_cast<std::uint32_t>(local_mappings.size());
        bounded(schema, "minLength", node.minimum_length, node.has_minimum_length);
        bounded(schema, "maxLength", node.maximum_length, node.has_maximum_length);
        bounded(schema, "minItems", node.minimum_items, node.has_minimum_items);
        bounded(schema, "maxItems", node.maximum_items, node.has_maximum_items);
        bounded(schema, "maxProperties", node.maximum_properties, node.has_maximum_properties);
        signed_bound(schema, "minimum", node.minimum, node.has_minimum);
        signed_bound(schema, "maximum", node.maximum, node.has_maximum);
        node.unique_items = schema.value("uniqueItems", false);
        nodes[index] = node;
    }

    static Pattern pattern_from(std::string_view value) {
        const auto generated = pattern_name(value);
        static const std::map<std::string_view, Pattern> values{
            {"Pattern::none", Pattern::none},
            {"Pattern::lowercase_sha256", Pattern::lowercase_sha256},
            {"Pattern::sampler_name", Pattern::sampler_name},
            {"Pattern::printable_text", Pattern::printable_text},
            {"Pattern::media_extension", Pattern::media_extension},
            {"Pattern::package_extension", Pattern::package_extension},
            {"Pattern::identifier", Pattern::identifier},
            {"Pattern::midi_manufacturer_id", Pattern::midi_manufacturer_id},
            {"Pattern::receive_channel", Pattern::receive_channel},
            {"Pattern::api_path", Pattern::api_path},
            {"Pattern::archive_content_path", Pattern::archive_content_path},
        };
        return values.at(generated);
    }

    std::uint32_t compile_reference(const std::string &reference) {
        const auto name = reference.substr(reference_prefix.size());
        if (!name.contains('/')) {
            const auto found = names.find(name);
            if (found == names.end())
                throw std::runtime_error{"unknown OpenAPI schema reference: " + name};
            return found->second;
        }

        const auto existing = reference_nodes.find(reference);
        if (existing != reference_nodes.end())
            return existing->second;

        const auto index = static_cast<std::uint32_t>(nodes.size());
        reference_nodes.emplace(reference, index);
        nodes.emplace_back();
        try {
            compile_at(index, document.at(Json::json_pointer{reference.substr(1U)}), reference);
        } catch (const Json::exception &error) {
            throw std::runtime_error{"unknown OpenAPI schema reference " + reference + ": " + error.what()};
        }
        return index;
    }

    Literal literal(const Json &value, std::string_view path) {
        if (value.is_null())
            return {LiteralKind::null, {}, 0, false};
        if (value.is_boolean())
            return {LiteralKind::boolean, {}, 0, value.get<bool>()};
        if (value.is_number_integer() || value.is_number_unsigned())
            return {LiteralKind::integer, {}, value.get<std::int64_t>(), false};
        if (value.is_string())
            return {LiteralKind::string, intern(value.get_ref<const std::string &>()), 0, false};
        throw std::runtime_error{"unsupported enum or const literal at " + std::string{path}};
    }

    template <typename T> static std::uint32_t append(std::vector<T> &target, const std::vector<T> &values) {
        const auto begin = static_cast<std::uint32_t>(target.size());
        target.insert(target.end(), values.begin(), values.end());
        return begin;
    }

    static void bounded(const Json &schema, std::string_view keyword, std::uint64_t &value, bool &present) {
        if (const auto found = schema.find(keyword); found != schema.end()) {
            value = found->get<std::uint64_t>();
            present = true;
        }
    }

    static void signed_bound(const Json &schema, std::string_view keyword, std::int64_t &value, bool &present) {
        if (const auto found = schema.find(keyword); found != schema.end()) {
            value = found->get<std::int64_t>();
            present = true;
        }
    }

    std::string_view intern(std::string_view value) {
        strings.emplace_back(value);
        return strings.back();
    }

    const Json &document;
    std::map<std::string, std::uint32_t, std::less<>> names;
    std::map<std::string, std::uint32_t, std::less<>> reference_nodes;
    std::vector<SchemaNode> nodes;
    std::vector<Property> properties;
    std::vector<std::uint32_t> variants;
    std::vector<Literal> literals;
    std::vector<EnumMapping> mappings;
    std::vector<NamedSchema> named;
    std::deque<std::string> strings;
};

std::string bool_text(bool value) { return value ? "true" : "false"; }

void write_output(std::ostream &out, const Compiler &value) {
    out << "#include \"validation_plan.hpp\"\n\n#include <array>\n\nnamespace axk::server::detail {\nnamespace {\n";
    out << "constexpr std::array<SchemaNode," << value.nodes.size() << "> nodes{\n";
    for (const auto &node : value.nodes) {
        out << "SchemaNode{" << unsigned{node.types} << ", Pattern::";
        const auto pattern = static_cast<unsigned>(node.pattern);
        static constexpr std::string_view names[]{"none",           "lowercase_sha256",     "sampler_name",
                                                  "printable_text", "media_extension",      "package_extension",
                                                  "identifier",     "midi_manufacturer_id", "receive_channel",
                                                  "api_path",       "archive_content_path"};
        out << names[pattern] << "," << node.reference << 'U' << ',' << node.items << 'U' << ','
            << node.additional_properties << 'U' << ',' << node.properties_begin << 'U' << ',' << node.properties_count
            << 'U' << ',' << node.variants_begin << 'U' << ',' << node.variants_count << 'U' << ','
            << node.literals_begin << 'U' << ',' << node.literals_count << 'U' << ',' << node.mappings_begin << 'U'
            << ',' << node.mappings_count << 'U' << ',' << node.minimum_length << "ULL," << node.maximum_length
            << "ULL," << node.minimum_items << "ULL," << node.maximum_items << "ULL," << node.maximum_properties
            << "ULL," << node.minimum << "LL," << node.maximum << "LL," << bool_text(node.additional_properties_allowed)
            << ',' << bool_text(node.unique_items) << ',' << bool_text(node.has_minimum_length) << ','
            << bool_text(node.has_maximum_length) << ',' << bool_text(node.has_minimum_items) << ','
            << bool_text(node.has_maximum_items) << ',' << bool_text(node.has_maximum_properties) << ','
            << bool_text(node.has_minimum) << ',' << bool_text(node.has_maximum) << "},\n";
    }
    out << "};\nconstexpr std::array<Property," << value.properties.size() << "> properties{\n";
    for (const auto &property : value.properties)
        out << "Property{" << quote(property.name) << ',' << property.schema << "U," << bool_text(property.required)
            << "},\n";
    out << "};\nconstexpr std::array<std::uint32_t," << value.variants.size() << "> variants{";
    for (const auto variant : value.variants)
        out << variant << "U,";
    out << "};\nconstexpr std::array<Literal," << value.literals.size() << "> literals{\n";
    for (const auto &literal : value.literals) {
        static constexpr std::string_view kinds[]{"null", "boolean", "integer", "string"};
        out << "Literal{LiteralKind::" << kinds[static_cast<unsigned>(literal.kind)] << ',' << quote(literal.text)
            << ',' << literal.integer << "LL," << bool_text(literal.boolean) << "},\n";
    }
    out << "};\nconstexpr std::array<EnumMapping," << value.mappings.size() << "> mappings{\n";
    for (const auto &mapping : value.mappings)
        out << "EnumMapping{" << quote(mapping.wire) << ',' << quote(mapping.application) << "},\n";
    out << "};\nconstexpr std::array<NamedSchema," << value.named.size() << "> schemas{\n";
    for (const auto &schema : value.named)
        out << "NamedSchema{" << quote(schema.name) << ',' << schema.schema << "U},\n";
    out << "};\nconstexpr ValidationPlan plan{nodes,properties,variants,literals,mappings,schemas};\n"
           "} // namespace\nconst ValidationPlan &generated_validation_plan() { return plan; }\n"
           "} // namespace axk::server::detail\n";
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: axk-server-validation-plan-generator OPENAPI OUTPUT\n";
        return 2;
    }
    try {
        std::ifstream input{std::filesystem::path{argv[1]}, std::ios::binary};
        if (!input)
            throw std::runtime_error{"could not open OpenAPI document"};
        const Json document = Json::parse(input);
        const Compiler compiler{document};
        std::ostringstream generated;
        write_output(generated, compiler);
        std::ofstream output{std::filesystem::path{argv[2]}, std::ios::binary | std::ios::trunc};
        output << generated.str();
        if (!output)
            throw std::runtime_error{"could not write validation plan"};
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "OpenAPI validation-plan generation failed: " << error.what() << '\n';
        return 1;
    }
}
