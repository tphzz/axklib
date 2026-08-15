#include "axklib/application/file_operations.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/media.hpp"
#include "axklib/relationship.hpp"
#include "axklib/report.hpp"
#include "axklib/semantic.hpp"
#include "axklib/terminology.hpp"
#include "axklib/utf8.hpp"
#include "axklib/version.hpp"

#include "file_operations_internal.hpp"

namespace axk::app::file_operations_internal {

std::vector<axk::ReportRow> program_detail_rows(std::span<const LoadedSource> sources,
                                                const axk::app::OperationContext &context) {
    std::vector<axk::ReportRow> rows;
    for (const auto &source : sources) {
        const auto display_path = source_display_path(source.source, context);
        for (const auto &relation : source.graph.relationships) {
            if (!relation.type.starts_with("PROG_ASSIGNMENT_TO_") || !relation.assignment_index)
                continue;
            const auto *program_item = catalog_object(source, relation.source_key);
            if (program_item == nullptr)
                continue;
            const auto *program = std::get_if<axk::CurrentProg>(&program_item->object.payload);
            if (program == nullptr || *relation.assignment_index >= program->assignments.size())
                continue;
            const auto &assignment = program->assignments[*relation.assignment_index];
            const auto *target = relation.target_key ? catalog_object(source, *relation.target_key) : nullptr;
            const auto *program_media = media_object(source, program_item->key);
            const auto *target_media = target == nullptr ? nullptr : media_object(source, target->key);
            const auto *program_fat = fat_file_metadata(source, program_media);
            const auto *target_fat = fat_file_metadata(source, target_media);
            const bool sfs = source.media.kind() == axk::MediaKind::sfs;
            const bool iso = source.media.kind() == axk::MediaKind::iso9660;
            const bool fat = source.media.kind() == axk::MediaKind::fat12_floppy;
            std::vector<std::string> candidate_keys;
            std::vector<std::string> candidate_files;
            std::vector<std::string> candidate_names;
            std::vector<std::string> candidate_categories;
            for (const auto &key : relation.candidate_keys) {
                candidate_keys.push_back(public_object_key(source, key));
                if (const auto *candidate = catalog_object(source, key)) {
                    candidate_names.push_back(candidate->object.header.name);
                    candidate_categories.push_back(object_type_name(candidate->object.header.type));
                }
                if (const auto *object = media_object(source, key))
                    candidate_files.push_back(object->logical_path);
            }
            std::ranges::sort(candidate_categories);
            candidate_categories.erase(std::unique(candidate_categories.begin(), candidate_categories.end()),
                                       candidate_categories.end());
            axk::ReportValue child_count{nullptr};
            if (target != nullptr && target->object.header.type == axk::ObjectType::sbac) {
                child_count =
                    static_cast<std::uint64_t>(std::ranges::count_if(source.graph.relationships, [&](const auto &row) {
                        return row.source_key == target->key && row.type == "SBAC_SLOT_TO_SBNK" &&
                               row.target_key.has_value();
                    }));
            }
            const auto expected = assignment.kind == 0x11U ? "SBAC" : assignment.kind == 0x10U ? "SBNK" : "";
            rows.push_back({
                {"image", display_path},
                {"container_kind", info_media_kind_name(source.media.kind())},
                {"scope_key", public_scope_key(source, *program_item, display_path)},
                {"prog_object_key", public_object_key(source, program_item->key)},
                {"prog_partition_index", optional_unsigned(sfs, program_item->partition.value)},
                {"prog_sfs_id", optional_unsigned(sfs, program_item->sfs_id.value)},
                {"prog_fat_file", fat && program_media != nullptr ? program_media->logical_path : ""},
                {"prog_payload_offset",
                 optional_unsigned(sfs || program_media != nullptr,
                                   sfs ? sfs_payload_offset(source, *program_item) : program_media->data_offset)},
                {"prog_name", program_item->object.header.name},
                {"prog_payload_size", program_media != nullptr ? program_media->size : std::uint64_t{0}},
                {"assignment_index", static_cast<std::uint64_t>(*relation.assignment_index)},
                {"assignment_offset", static_cast<std::uint64_t>(0x120U + *relation.assignment_index * 0x38U)},
                {"assignment_name", assignment.name},
                {"assignment_raw_handle_0x10", static_cast<std::uint64_t>(assignment.raw_handle)},
                {"assignment_kind_byte_0x14", static_cast<std::uint64_t>(assignment.kind)},
                {"assignment_flag_byte_0x15", static_cast<std::uint64_t>(assignment.flags)},
                {"assignment_output1_byte_0x1d",
                 static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(assignment.raw_row[0x1d]))},
                {"assignment_output2_byte_0x28",
                 static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(assignment.raw_row[0x28]))},
                {"assignment_rch_assign_display", relation.receive_channel_display},
                {"selector_expected_category", expected},
                {"assignment_row_state", "decoded-row"},
                {"assignment_state", std::string{axk::assignment_state_name(relation.assignment_state)}},
                {"match_method", relation.basis},
                {"match_quality", std::string{axk::relationship_quality_name(relation.quality)}},
                {"match_notes", relation.notes},
                {"candidate_count", static_cast<std::uint64_t>(relation.candidate_keys.size())},
                {"candidate_categories", joined_strings(candidate_categories)},
                {"candidate_object_keys", joined_strings(candidate_keys)},
                {"candidate_fat_files", fat ? joined_strings(candidate_files) : ""},
                {"candidate_names", joined_strings(candidate_names)},
                {"matched_target_type", target == nullptr ? "" : object_type_name(target->object.header.type)},
                {"matched_target_object_key", target == nullptr ? "" : public_object_key(source, target->key)},
                {"matched_target_partition_index",
                 optional_unsigned(sfs && target != nullptr, target == nullptr ? 0U : target->partition.value)},
                {"matched_target_sfs_id",
                 optional_unsigned(sfs && target != nullptr, target == nullptr ? 0U : target->sfs_id.value)},
                {"matched_target_fat_file", fat && target_media != nullptr ? target_media->logical_path : ""},
                {"matched_target_payload_offset",
                 optional_unsigned(target != nullptr && (sfs || target_media != nullptr),
                                   target == nullptr ? 0U
                                   : sfs             ? sfs_payload_offset(source, *target)
                                                     : target_media->data_offset)},
                {"matched_target_name", target == nullptr ? "" : target->object.header.name},
                {"matched_sbac_child_sbnk_count", child_count},
                {"notes", ""},
                {"prog_iso_extent_sector",
                 optional_unsigned(iso && program_media != nullptr,
                                   program_media == nullptr ? 0U : program_media->data_offset / 2048U)},
                {"prog_iso_data_offset", optional_unsigned(iso && program_media != nullptr,
                                                           program_media == nullptr ? 0U : program_media->data_offset)},
                {"prog_iso_file_size", optional_unsigned(iso && program_media != nullptr,
                                                         program_media == nullptr ? 0U : program_media->size)},
                {"prog_iso_recovery_quality", iso ? "clean-iso9660-object" : ""},
                {"prog_fat_directory_offset",
                 optional_unsigned(program_fat != nullptr,
                                   program_fat == nullptr ? 0U : program_fat->directory_offset)},
                {"prog_fat_first_cluster",
                 optional_unsigned(program_fat != nullptr, program_fat == nullptr ? 0U : program_fat->first_cluster)},
                {"prog_fat_cluster_count",
                 optional_unsigned(program_fat != nullptr, program_fat == nullptr ? 0U : program_fat->clusters.size())},
                {"prog_fat_file_size", optional_unsigned(fat && program_media != nullptr,
                                                         program_media == nullptr ? 0U : program_media->size)},
                {"prog_fat_object_offset",
                 optional_unsigned(fat && program_media != nullptr,
                                   program_media == nullptr ? 0U : program_media->data_offset)},
                {"prog_fat_stored_payload_offset",
                 optional_unsigned(program_fat != nullptr,
                                   program_fat == nullptr ? 0U : program_fat->first_data_offset)},
                {"matched_target_iso_extent_sector",
                 optional_unsigned(iso && target_media != nullptr,
                                   target_media == nullptr ? 0U : target_media->data_offset / 2048U)},
                {"matched_target_iso_data_offset",
                 optional_unsigned(iso && target_media != nullptr,
                                   target_media == nullptr ? 0U : target_media->data_offset)},
                {"matched_target_iso_file_size",
                 optional_unsigned(iso && target_media != nullptr, target_media == nullptr ? 0U : target_media->size)},
                {"matched_target_iso_recovery_quality", iso && target_media != nullptr ? "clean-iso9660-object" : ""},
                {"matched_target_fat_directory_offset",
                 optional_unsigned(target_fat != nullptr, target_fat == nullptr ? 0U : target_fat->directory_offset)},
                {"matched_target_fat_first_cluster",
                 optional_unsigned(target_fat != nullptr, target_fat == nullptr ? 0U : target_fat->first_cluster)},
                {"matched_target_fat_cluster_count",
                 optional_unsigned(target_fat != nullptr, target_fat == nullptr ? 0U : target_fat->clusters.size())},
                {"matched_target_fat_file_size",
                 optional_unsigned(fat && target_media != nullptr, target_media == nullptr ? 0U : target_media->size)},
                {"matched_target_fat_object_offset",
                 optional_unsigned(fat && target_media != nullptr,
                                   target_media == nullptr ? 0U : target_media->data_offset)},
                {"matched_target_fat_stored_payload_offset",
                 optional_unsigned(target_fat != nullptr, target_fat == nullptr ? 0U : target_fat->first_data_offset)},
            });
        }
    }
    return rows;
}

std::vector<axk::ReportRow> program_ignored_detail_rows(std::span<const LoadedSource> sources,
                                                        const axk::app::OperationContext &context) {
    std::vector<axk::ReportRow> rows;
    for (const auto &source : sources) {
        const auto display_path = source_display_path(source.source, context);
        for (const auto &item : source.inventory.catalog.objects) {
            const auto *program = std::get_if<axk::CurrentProg>(&item.object.payload);
            if (program == nullptr)
                continue;
            std::set<std::size_t> represented;
            for (const auto &relation : source.graph.relationships) {
                if (relation.source_key == item.key && relation.type.starts_with("PROG_ASSIGNMENT_TO_") &&
                    relation.assignment_index) {
                    represented.insert(*relation.assignment_index);
                }
            }
            const auto *program_media = media_object(source, item.key);
            const bool sfs = source.media.kind() == axk::MediaKind::sfs;
            const bool fat = source.media.kind() == axk::MediaKind::fat12_floppy;
            for (std::size_t index = 0; index < program->assignments.size(); ++index) {
                const auto &assignment = program->assignments[index];
                if (assignment.name.empty() || represented.contains(index))
                    continue;
                const bool known_kind = assignment.kind == 0x10U || assignment.kind == 0x11U;
                const bool name_match = std::ranges::any_of(source.inventory.catalog.objects, [&](const auto &target) {
                    return target.scope_key == item.scope_key && target.object.header.name == assignment.name;
                });
                std::string reason;
                if (!known_kind && !name_match) {
                    reason = "ignored-reserved-or-tail-slot-no-known-kind-and-no-name-match";
                } else if (assignment.raw_handle == 0U) {
                    reason = "ignored-null-handle-unmatched-assignment";
                } else {
                    continue;
                }
                rows.push_back({
                    {"image", display_path},
                    {"container_kind", info_media_kind_name(source.media.kind())},
                    {"scope_key", public_scope_key(source, item, display_path)},
                    {"prog_object_key", public_object_key(source, item.key)},
                    {"prog_partition_index", optional_unsigned(sfs, item.partition.value)},
                    {"prog_sfs_id", optional_unsigned(sfs, item.sfs_id.value)},
                    {"prog_fat_file", fat && program_media != nullptr ? program_media->logical_path : ""},
                    {"prog_payload_offset", optional_unsigned(sfs || program_media != nullptr,
                                                              sfs ? sfs_payload_offset(source, item)
                                                              : program_media == nullptr ? 0U
                                                                                         : program_media->data_offset)},
                    {"prog_name", item.object.header.name},
                    {"prog_payload_size", program_media == nullptr ? std::uint64_t{0} : program_media->size},
                    {"assignment_index", static_cast<std::uint64_t>(index)},
                    {"assignment_offset", static_cast<std::uint64_t>(0x120U + index * 0x38U)},
                    {"raw_name_guess", assignment.name},
                    {"assignment_raw_handle_0x10", static_cast<std::uint64_t>(assignment.raw_handle)},
                    {"assignment_kind_byte_0x14", static_cast<std::uint64_t>(assignment.kind)},
                    {"assignment_flag_byte_0x15", static_cast<std::uint64_t>(assignment.flags)},
                    {"reason", std::move(reason)},
                });
            }
        }
    }
    return rows;
}

axk::ReportRow coverage_summary(const std::vector<LoadedSource> &sources, std::span<const axk::ReportRow> relationships,
                                std::size_t load_error_count) {
    std::map<std::string, std::uint64_t> qualities;
    std::map<std::string, std::uint64_t> types;
    std::uint64_t sbac{};
    std::uint64_t program{};
    std::uint64_t bitmaps{};
    std::uint64_t ignored{};
    for (const auto &source : sources) {
        for (const auto &row : source.graph.relationships) {
            ++qualities[std::string{axk::relationship_quality_name(row.quality)}];
            ++types[row.type];
            if (row.type == "SBAC_SLOT_TO_SBNK")
                ++sbac;
            if (row.type.starts_with("PROG_ASSIGNMENT_TO_"))
                ++program;
        }
        bitmaps += source.graph.bitmap_comparisons.size();
        ignored += program_ignored_count(source);
    }
    const auto joined = [](const auto &counts) {
        std::string result;
        for (const auto &[name, count] : counts) {
            if (count == 0U)
                continue;
            if (!result.empty())
                result += ';';
            result += std::format("{}:{}", name, count);
        }
        return result;
    };
    return {{"relationship_count", static_cast<std::uint64_t>(relationships.size())},
            {"known_relationship_count", qualities["Known"]},
            {"likely_relationship_count", qualities["Likely"]},
            {"tentative_relationship_count", qualities["Tentative"]},
            {"unknown_relationship_count", qualities["Unknown"]},
            {"ambiguous_relationship_count", qualities["Tentative"]},
            {"sbac_sbnk_row_count", sbac},
            {"prog_assignment_row_count", program},
            {"prog_ignored_row_count", ignored},
            {"sbnk_bitmap_row_count", bitmaps},
            {"relationship_type_counts", joined(types)},
            {"quality_counts", joined(qualities)},
            {"load_error_count", static_cast<std::uint64_t>(load_error_count)}};
}

axk::app::Result<Json> execute_coverage(const axk::app::Sandbox &sandbox, const Json &input,
                                        const axk::app::OperationContext &context) {
    const auto request = parse_request(input);
    if (!request)
        return std::unexpected(request.error());
    const auto destination = sandbox.create_staging_directory("axklib-report-coverage");
    if (!destination)
        return std::unexpected(destination.error());
    DirectoryCleanup cleanup{*destination};
    std::error_code filesystem_error;
    std::filesystem::create_directories(*destination / "_schemas", filesystem_error);
    if (filesystem_error) {
        return std::unexpected(operation_error("report_output_failed", "could not create report output directory",
                                               request->destination.relative_path));
    }
    std::vector<LoadedSource> loaded;
    std::vector<axk::ReportRow> load_errors;
    for (std::size_t index = 0; index < request->sources.size(); ++index) {
        const auto &source_ref = request->sources[index];
        if (context.progress != nullptr) {
            context.progress->report(
                {axk::ProgressPhase::reading, index, request->sources.size(), source_ref.relative_path, std::nullopt});
        }
        auto source = load_info_source(sandbox, source_ref, request->include_default_programs, context);
        if (!source) {
            load_errors.push_back({{"path", source_display_path(source_ref, context)},
                                   {"error_code", source.error().error_code},
                                   {"message", source.error().error.message},
                                   {"recoverable", true},
                                   {"original_exception", source.error().original_exception}});
            continue;
        }
        loaded.push_back(std::move(*source));
    }
    std::vector<axk::ReportRow> relationships;
    for (const auto &source : loaded) {
        const auto display_path = source_display_path(source.source, context);
        for (const auto &row : source.graph.relationships)
            relationships.push_back(relationship_report_row(source, row, display_path));
    }
    auto summary = coverage_summary(loaded, relationships, load_errors.size());
    const std::array summary_rows{summary};
    if (auto written =
            axk::write_report_csv(*destination / "coverage_summary.csv", summary_rows, {}, request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "coverage_summary.csv")}));
    }
    if (auto written = axk::write_report_object(*destination / "coverage_summary.json", summary, request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "coverage_summary.json")}));
    }
    axk::ReportSchemaOptions summary_options;
    summary_options.source_command = "axklib";
    summary_options.library_version = std::string{axk::version()};
    auto summary_schema = axk::make_report_schema("coverage_summary", summary_rows, std::move(summary_options));
    if (auto written = axk::write_report_schema(*destination / "_schemas" / "coverage_summary.schema.json",
                                                summary_schema, request->overwrite);
        !written) {
        return std::unexpected(core_error(
            written.error(), {request->destination.root_id,
                              child_reference_path(request->destination, "_schemas/coverage_summary.schema.json")}));
    }
    auto relation_schema =
        write_report_set(*destination, request->destination, "relationships", relationships, {}, request->overwrite);
    if (!relation_schema)
        return std::unexpected(relation_schema.error());
    auto error_schema =
        write_report_set(*destination, request->destination, "load_errors", load_errors, {}, request->overwrite);
    if (!error_schema)
        return std::unexpected(error_schema.error());
    const std::array schemas{summary_schema, *relation_schema, *error_schema};
    if (auto written = axk::write_report_schema_index(*destination / "_schemas" / "schema_index.json", schemas,
                                                      request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "_schemas/schema_index.json")}));
    }
    auto artifacts = Json::array();
    for (const auto path :
         {"coverage_summary.csv", "coverage_summary.json", "relationships.csv", "relationships.json", "load_errors.csv",
          "load_errors.json", "_schemas/coverage_summary.schema.json", "_schemas/relationships.schema.json",
          "_schemas/load_errors.schema.json", "_schemas/schema_index.json"}) {
        artifacts.push_back({{"rootId", request->destination.root_id},
                             {"relativePath", child_reference_path(request->destination, path)}});
    }
    if (auto published = sandbox.publish_directory(request->destination, request->overwrite, *destination); !published)
        return std::unexpected(published.error());
    return Json{{"operationId", "report.coverage"}, {"sourceCount", request->sources.size()},
                {"loadedCount", loaded.size()},     {"failedCount", load_errors.size()},
                {"rowCount", relationships.size()}, {"artifacts", std::move(artifacts)}};
}

} // namespace axk::app::file_operations_internal
