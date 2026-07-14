#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>

#include "content_id.hpp"
#include "handlers.hpp"
#include "reports.hpp"
#include "requests.hpp"
#include "schema/info_v1.hpp"
#include "schema/operations_v1.hpp"
#include "support.hpp"

#include "axklib/alteration.hpp"
#include "axklib/audio.hpp"
#include "axklib/audio_export.hpp"
#include "axklib/catalog.hpp"
#include "axklib/error.hpp"
#include "axklib/media.hpp"
#include "axklib/object.hpp"
#include "axklib/relationship.hpp"
#include "axklib/report.hpp"
#include "axklib/semantic.hpp"
#include "axklib/sfs.hpp"
#include "axklib/utf8.hpp"
#include "axklib/version.hpp"
#include "axklib/writer.hpp"

namespace axk::cli::commands {

int run_relationships_request(const axk::cli::RelationshipsRequest &request) {
    if (const auto ready = prepare_report_directory(request.output_directory, request.overwrite); !ready)
        return report_failure(ready.error());
    const auto loaded = load_cli_paths(request.paths);
    const auto rows = relationship_rows(loaded);
    std::vector<axk::ReportSchemaManifest> schemas;
    auto relation_schema =
        write_cli_report(request.output_directory, "relationships", rows, "axklib relationships", request.overwrite);
    if (!relation_schema)
        return report_failure(relation_schema.error());
    schemas.push_back(*relation_schema);
    const std::array<std::pair<std::string_view, std::string_view>, 4> detail_reports{{
        {"current_sbac_sbnk_links", "SBAC_SLOT_TO_SBNK"},
        {"current_prog_bank_links", "PROG_ASSIGNMENT_TO_"},
        {"current_prog_ignored_reserved_or_tail", "never"},
        {"current_sbnk_program_bitmap_crosscheck", "SBNK_PROGRAM_BITMAP_TO_PROG"},
    }};
    for (const auto &[name, prefix] : detail_reports) {
        std::vector<axk::ReportRow> selected;
        if (name == "current_sbac_sbnk_links") {
            selected = sbac_detail_rows(loaded);
        } else if (name == "current_prog_bank_links") {
            selected = program_detail_rows(loaded);
        } else if (name == "current_prog_ignored_reserved_or_tail") {
            selected = program_ignored_detail_rows(loaded);
        } else if (name == "current_sbnk_program_bitmap_crosscheck") {
            selected = bitmap_detail_rows(loaded);
        } else {
            for (const auto &source : loaded.loaded) {
                for (const auto &row : source.graph.relationships) {
                    if ((prefix.ends_with('_') && row.type.starts_with(prefix)) || row.type == prefix)
                        selected.push_back(relationship_report_row(source, row));
                }
            }
        }
        auto schema = write_cli_report(request.output_directory, std::string{name}, selected, "axklib relationships",
                                       request.overwrite);
        if (!schema)
            return report_failure(schema.error());
        schemas.push_back(*schema);
    }
    auto load_schema = write_cli_report(request.output_directory, "load_errors", loaded.errors, "axklib relationships",
                                        request.overwrite);
    if (!load_schema)
        return report_failure(load_schema.error());
    schemas.push_back(*load_schema);
    auto summary = coverage_summary(loaded, rows);
    if (auto written = axk::write_report_object(request.output_directory / "relationship_summary.json", summary,
                                                request.overwrite);
        !written)
        return report_failure(written.error());
    axk::ReportSchemaOptions summary_options;
    summary_options.source_command = "axklib";
    summary_options.library_version = std::string{axk::version()};
    auto summary_schema = axk::make_report_schema("relationship_summary", std::span{&summary, 1U}, summary_options);
    schemas.push_back(summary_schema);
    if (auto written =
            axk::write_report_schema(request.output_directory / "_schemas" / "relationship_summary.schema.json",
                                     summary_schema, request.overwrite);
        !written)
        return report_failure(written.error());
    if (auto index = axk::write_report_schema_index(request.output_directory / "_schemas" / "schema_index.json",
                                                    schemas, request.overwrite);
        !index)
        return report_failure(index.error());
    std::cout << "relationships=" << rows.size() << " ambiguous="
              << std::ranges::count_if(rows,
                                       [](const auto &row) {
                                           return std::ranges::any_of(row, [](const auto &field) {
                                               return field.first == "quality" &&
                                                      std::get_if<std::string>(&field.second.value) != nullptr &&
                                                      std::get<std::string>(field.second.value) == "Tentative";
                                           });
                                       })
              << " load_errors=" << loaded.errors.size() << '\n';
    return loaded.errors.empty() ? 0 : 3;
}

int run_coverage_request(const axk::cli::CoverageRequest &request) {
    if (const auto ready = prepare_report_directory(request.output_directory, request.overwrite); !ready)
        return report_failure(ready.error());
    const auto loaded = load_cli_paths(request.paths);
    const auto rows = relationship_rows(loaded);
    auto summary = coverage_summary(loaded, rows);
    const std::array summary_rows{summary};
    if (auto written = axk::write_report_csv(request.output_directory / "coverage_summary.csv", summary_rows, {},
                                             request.overwrite);
        !written)
        return report_failure(written.error());
    if (auto written =
            axk::write_report_object(request.output_directory / "coverage_summary.json", summary, request.overwrite);
        !written)
        return report_failure(written.error());
    axk::ReportSchemaOptions summary_options;
    summary_options.source_command = "axklib";
    summary_options.library_version = std::string{axk::version()};
    auto summary_schema = axk::make_report_schema("coverage_summary", summary_rows, std::move(summary_options));
    if (auto written = axk::write_report_schema(request.output_directory / "_schemas" / "coverage_summary.schema.json",
                                                summary_schema, request.overwrite);
        !written)
        return report_failure(written.error());
    auto relation_schema =
        write_cli_report(request.output_directory, "relationships", rows, "axklib coverage", request.overwrite);
    if (!relation_schema)
        return report_failure(relation_schema.error());
    auto error_schema =
        write_cli_report(request.output_directory, "load_errors", loaded.errors, "axklib coverage", request.overwrite);
    if (!error_schema)
        return report_failure(error_schema.error());
    const std::array schemas{summary_schema, *relation_schema, *error_schema};
    if (auto index = axk::write_report_schema_index(request.output_directory / "_schemas" / "schema_index.json",
                                                    schemas, request.overwrite);
        !index)
        return report_failure(index.error());
    std::cout << "relationships=" << rows.size() << " load_errors=" << loaded.errors.size() << '\n';
    return loaded.errors.empty() ? 0 : 3;
}

axk::ContentTree cli_content_tree(const CliLoaded &loaded, bool include_default_programs) {
    return axk::build_content_tree(loaded.media, loaded.catalog, loaded.graph, include_default_programs);
}

std::string sfs_selector_component(const axk::ContentNode &node) {
    if (node.node_type == "partition") {
        const auto separator = node.node_id.find(':');
        const auto raw_index = separator == std::string::npos ? std::string{} : node.node_id.substr(separator + 1U);
        auto partition_name = node.display_name;
        const auto prefix = std::format("partition {}: ", raw_index);
        if (partition_name.starts_with(prefix))
            partition_name.erase(0U, prefix.size());
        std::string safe;
        bool prior_space{};
        for (const auto value : partition_name) {
            const auto byte = static_cast<unsigned char>(value);
            if (std::isspace(byte) != 0) {
                if (!safe.empty() && !prior_space)
                    safe.push_back('_');
                prior_space = true;
            } else {
                const bool retained = std::isalnum(byte) != 0 || value == '.' || value == '_' || value == '-';
                safe.push_back(retained ? value : '_');
                prior_space = false;
            }
        }
        while (!safe.empty() && (safe.front() == '.' || safe.front() == '_' || safe.front() == '-'))
            safe.erase(safe.begin());
        while (!safe.empty() && (safe.back() == '.' || safe.back() == '_' || safe.back() == '-'))
            safe.pop_back();
        return std::format("partition_{:0>2}_{}", raw_index,
                           safe.empty() ? std::format("partition_{:0>2}", raw_index) : safe);
    }
    auto result = node.display_name;
    std::ranges::replace(result, '/', '_');
    std::ranges::replace(result, '\\', '_');
    return result;
}

std::string first_media_object_directory(const CliLoaded &loaded, const axk::ContentNode &node) {
    if (!node.object_key.empty()) {
        const auto object = std::ranges::find(loaded.objects, node.object_key, &axk::MediaObject::key);
        if (object != loaded.objects.end()) {
            auto directory = std::filesystem::path{object->logical_path}.parent_path();
            if (loaded.media.kind() == axk::MediaKind::iso9660)
                directory = directory.parent_path();
            return axk::text::path_to_utf8(directory);
        }
    }
    for (const auto &child : node.children) {
        auto directory = first_media_object_directory(loaded, child);
        if (!directory.empty())
            return directory;
    }
    return {};
}

std::string selector_component(const CliLoaded &loaded, const axk::ContentNode &node) {
    if (loaded.media.kind() == axk::MediaKind::sfs)
        return sfs_selector_component(node);
    if (node.node_type == "partition" || node.node_type == "volume") {
        auto name = node.display_name;
        constexpr std::string_view error_suffix{" (errors detected)"};
        if (node.node_type == "volume" && name.ends_with(error_suffix))
            name.resize(name.size() - error_suffix.size());
        return axk::sanitize_path_component(name, node.node_type);
    }
    auto component = node.display_name;
    std::ranges::replace(component, '/', '_');
    std::ranges::replace(component, '\\', '_');
    const auto first = component.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return node.node_type;
    const auto last = component.find_last_not_of(" \t\r\n");
    return component.substr(first, last - first + 1U);
}

axk::cli::schema::info_v1::NodeOutput compatible_tree_node_output(const CliLoaded &loaded, const axk::ContentNode &node,
                                                                  std::string parent_path = {},
                                                                  std::string parent_id = {},
                                                                  std::string parent_type = {}) {
    const auto component = selector_component(loaded, node);
    const auto selector = parent_path.empty() ? component : std::format("{}/{}", parent_path, component);
    std::vector<axk::cli::schema::info_v1::NodeOutput> children;
    const auto object_key = node.object_key.empty() ? std::string{} : public_object_key(loaded, node.object_key);
    auto node_id = node.node_id;
    if (!node.object_key.empty())
        node_id = std::format("object:{}", object_key);
    else if ((loaded.media.kind() == axk::MediaKind::fat12_floppy ||
              loaded.media.kind() == axk::MediaKind::standalone_object) &&
             node.node_type == "volume") {
        node_id = std::format("scope:{}", node.display_name);
    } else if (loaded.media.kind() == axk::MediaKind::iso9660 && node.node_type == "partition") {
        node_id = "partition:None";
    } else if (loaded.media.kind() == axk::MediaKind::iso9660 && node.node_type == "volume") {
        node_id = std::format("volume:None:{}", first_media_object_directory(loaded, node));
    } else if (loaded.media.kind() == axk::MediaKind::iso9660 && node.node_type == "category" &&
               parent_type == "volume") {
        const auto volume_prefix = parent_id.find(':');
        const auto volume_tail =
            volume_prefix == std::string::npos ? std::string{} : parent_id.substr(volume_prefix + 1U);
        node_id = std::format("category:{}:{}", volume_tail, node.display_name);
    } else if (loaded.media.kind() == axk::MediaKind::sfs && node.node_type == "volume") {
        const auto separator = parent_id.find(':');
        const auto partition_index = separator == std::string::npos ? std::string{} : parent_id.substr(separator + 1U);
        node_id = std::format("volume:{}:{}", partition_index, node.display_name);
    } else if (loaded.media.kind() == axk::MediaKind::sfs && node.node_type == "category" && parent_type == "volume") {
        const auto volume_prefix = parent_id.find(':');
        const auto volume_tail =
            volume_prefix == std::string::npos ? std::string{} : parent_id.substr(volume_prefix + 1U);
        node_id = std::format("category:{}:{}", volume_tail, node.display_name);
    }
    for (const auto &child : node.children)
        children.push_back(compatible_tree_node_output(loaded, child, selector, node_id, node.node_type));
    const bool counted = node.node_type == "partition" || node.node_type == "volume" || node.node_type == "category";
    auto notes = node.notes;
    if (parent_type == "sample_bank" && node.object_type == "SBNK" && node.quality == axk::RelationshipQuality::known) {
        notes = "Input consistency: counted SBAC slot name uniquely matches a "
                "same-scope SBNK "
                "header name. The companion 32-bit slot word is preserved as "
                "raw/opaque.";
    }
    return {
        .node_id = std::move(node_id),
        .node_type = node.node_type,
        .display_name = node.display_name,
        .object_key = object_key,
        .object_type = node.object_type,
        .count = counted ? std::optional<std::uint64_t>{node.children.size()} : std::nullopt,
        .details = node.details,
        .quality = std::string{axk::relationship_quality_name(node.quality)},
        .basis = node.basis,
        .notes = std::move(notes),
        .selector_path = selector,
        .children = std::move(children),
    };
}

std::string tree_type_label(const axk::ContentNode &node) {
    if (node.node_type == "partition")
        return "PARTITION";
    if (node.node_type == "volume")
        return "VOLUME";
    if (node.node_type == "category")
        return "CATEGORY";
    if (node.node_type == "recovery_artifact")
        return "RECOVERY";
    if (node.node_type == "unresolved" || node.node_type == "relationship_target" ||
        node.node_type == "unresolved_program_assignment")
        return "UNKNOWN";
    if (node.object_type == "PROG")
        return "PROGRAM";
    if (node.object_type == "SBAC")
        return "SAMPLE BANK GROUP";
    if (node.object_type == "SBNK")
        return "SAMPLE BANK";
    if (node.object_type == "SMPL")
        return "WAVEFORM";
    if (node.object_type == "SEQU")
        return "SEQUENCE";
    return {};
}

void render_tree_text(const axk::ContentNode &node, std::size_t depth, std::string prefix, bool last,
                      const axk::cli::InfoRequest &request) {
    if (request.max_depth && depth > *request.max_depth)
        return;
    std::cout << prefix << (last ? "`-- " : "|-- ") << node.display_name;
    const auto label = tree_type_label(node);
    if (!label.empty())
        std::cout << " [" << label << ']';
    if (node.node_type == "partition" || node.node_type == "volume" || node.node_type == "category")
        std::cout << " (" << node.children.size() << ')';
    if (!node.details.empty()) {
        std::cout << " - ";
        for (std::size_t index = 0; index < node.details.size(); ++index) {
            if (index != 0U)
                std::cout << "; ";
            std::cout << node.details[index];
        }
    }
    if (request.show_quality || node.quality != axk::RelationshipQuality::known)
        std::cout << " [" << axk::relationship_quality_name(node.quality) << ']';
    if (!node.notes.empty() &&
        (node.quality == axk::RelationshipQuality::unknown || node.quality == axk::RelationshipQuality::tentative))
        std::cout << " - " << node.notes;
    std::cout << '\n';
    const auto child_prefix = prefix + (last ? "    " : "|   ");
    for (std::size_t index = 0; index < node.children.size(); ++index)
        render_tree_text(node.children[index], depth + 1U, child_prefix, index + 1U == node.children.size(), request);
}

void render_tree_paths(const CliLoaded &loaded, const axk::ContentNode &node, std::string parent_path = {}) {
    const auto component = selector_component(loaded, node);
    const auto selector = parent_path.empty() ? component : std::format("{}/{}", parent_path, component);
    std::string scope;
    if (node.node_type == "volume")
        scope = "volume";
    else if (node.object_type == "PROG")
        scope = "program";
    else if (node.object_type == "SBAC")
        scope = "sbac";
    else if (node.object_type == "SBNK")
        scope = "sbnk";
    if (!scope.empty()) {
        std::cout << axk::text::path_to_utf8(loaded.path) << '\t' << scope << '\t' << selector << '\t'
                  << node.display_name << '\t' << node.object_type << '\t'
                  << (node.object_key.empty() ? std::string{} : public_object_key(loaded, node.object_key)) << '\n';
    }
    for (const auto &child : node.children)
        render_tree_paths(loaded, child, selector);
}

int run_info_request(const axk::cli::InfoRequest &request) {
    const auto loaded = load_cli_paths(request.paths);
    if (request.format == "json") {
        axk::cli::schema::info_v1::InfoOutput output;
        for (const auto &source : loaded.loaded) {
            const auto tree = cli_content_tree(source, request.show_default_programs);
            axk::cli::schema::info_v1::TreeOutput projected{
                .source_path_utf8 = axk::text::path_to_utf8(source.path),
                .container_kind = media_kind_text(source.media.kind()),
                .detected_format = media_kind_text(source.media.kind()),
                .roots = {},
                .issues = {},
            };
            for (const auto &root : tree.roots)
                projected.roots.push_back(compatible_tree_node_output(source, root));
            for (const auto &issue : tree.issues) {
                projected.issues.push_back({
                    .code = issue.code,
                    .severity = issue.severity,
                    .message = issue.message,
                    .source_path_utf8 = axk::text::path_to_utf8(source.path),
                    .sampler_path = issue.sampler_path,
                    .object_key =
                        issue.object_key.empty() ? std::string{} : public_object_key(source, issue.object_key),
                });
            }
            output.trees.push_back(std::move(projected));
        }
        for (const auto &row : loaded.errors) {
            axk::cli::schema::info_v1::LoadErrorOutput projected;
            for (const auto &[name, item] : row) {
                if (name == "path")
                    projected.path_utf8 = std::get<std::string>(item.value);
                else if (name == "error_code")
                    projected.error_code = std::get<std::uint64_t>(item.value);
                else if (name == "message")
                    projected.message = std::get<std::string>(item.value);
                else if (name == "original_exception")
                    projected.original_exception = std::get<std::string>(item.value);
            }
            output.load_errors.push_back(std::move(projected));
        }
        const auto serialized = axk::cli::schema::info_v1::serialize(output);
        if (!serialized)
            return report_failure(serialized.error());
        std::cout << *serialized << '\n';
        return loaded.errors.empty() ? 0 : 1;
    }
    for (const auto &error : loaded.errors) {
        const auto path = std::get<std::string>(error[0].second.value);
        const auto message = std::get<std::string>(error[2].second.value);
        std::cout << path << "\tERROR\tAXKLIB_CONTAINER_OPEN_FAILED\t" << message << '\n';
    }
    if (request.format == "summary") {
        for (const auto &source : loaded.loaded) {
            std::map<std::string, std::size_t> counts;
            for (const auto &item : source.catalog.objects)
                ++counts[object_type_text(item.object.header.type)];
            std::cout << axk::text::path_to_utf8(source.path) << '\t' << media_kind_text(source.media.kind())
                      << "\tobjects=" << source.catalog.objects.size();
            for (const auto &[type, count] : counts)
                std::cout << ' ' << type << '=' << count;
            if (source.media.kind() == axk::MediaKind::iso9660)
                std::cout << "\trecovery=clean-iso9660-object:" << source.catalog.objects.size() << '\n';
            else
                std::cout << "\trecovery=-\n";
        }
        return loaded.errors.empty() ? 0 : 1;
    }
    for (const auto &source : loaded.loaded) {
        const auto tree = cli_content_tree(source, request.show_default_programs);
        if (request.format == "paths")
            std::cout << "source_path\tscope\tpath\tdisplay_name\tobject_"
                         "type\tobject_key\n";
        else
            std::cout << axk::text::path_to_utf8(source.path) << " [" << media_kind_text(source.media.kind()) << "]\n";
        for (const auto &root : tree.roots) {
            if (request.format == "paths")
                render_tree_paths(source, root);
            else
                render_tree_text(root, 1U, {}, root.node_id == tree.roots.back().node_id, request);
        }
    }
    return loaded.errors.empty() ? 0 : 1;
}

} // namespace axk::cli::commands
