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

#include <nlohmann/json.hpp>

#include "content_id.hpp"
#include "handlers.hpp"
#include "reports.hpp"
#include "requests.hpp"
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

using Json = nlohmann::json;

int run_objects_request(const axk::cli::ObjectsRequest &request) {
  if (!request.output_directory)
    return 2;
  if (const auto ready = prepare_report_directory(*request.output_directory, request.overwrite);
      !ready)
    return report_failure(ready.error());
  const auto loaded = load_cli_paths(request.paths);
  std::vector<axk::ReportRow> rows;
  for (const auto &source : loaded.loaded) {
    for (const auto &item : source.catalog.objects) {
      if (request.object_type && object_type_text(item.object.header.type) != *request.object_type)
        continue;
      rows.push_back(inventory_row(source, item));
    }
  }
  auto schema = write_cli_report(*request.output_directory, "objects", rows, "axklib objects",
                                 request.overwrite);
  if (!schema)
    return report_failure(schema.error());
  const std::array schemas{*schema};
  if (auto index = axk::write_report_schema_index(
          *request.output_directory / "_schemas" / "schema_index.json", schemas, request.overwrite);
      !index)
    return report_failure(index.error());
  std::cout << "objects=" << rows.size() << " load_errors=" << loaded.errors.size() << '\n';
  std::cout << "reports written to " << axk::text::path_to_utf8(*request.output_directory) << '\n';
  return loaded.errors.empty() ? 0 : 3;
}

int run_inventory_request(const axk::cli::InventoryRequest &request) {
  if (const auto ready = prepare_report_directory(request.output_directory, request.overwrite);
      !ready)
    return report_failure(ready.error());
  const auto loaded = load_cli_paths(request.paths);
  std::vector<axk::ReportRow> rows;
  std::vector<axk::ReportRow> issues;
  std::map<std::string, std::uint64_t> counts;
  for (const auto &source : loaded.loaded) {
    for (const auto &item : source.catalog.objects) {
      rows.push_back(inventory_row(source, item));
      ++counts[object_type_text(item.object.header.type)];
    }
    for (const auto &issue : source.catalog.issues) {
      issues.push_back(
          {{"source_path", axk::text::path_to_utf8(source.path)},
           {"container_kind", media_kind_text(source.media.kind())},
           {"object_key", issue.sfs_id
                              ? std::format("p{}:sfs{}", issue.partition.value, issue.sfs_id->value)
                              : std::string{}},
           {"object_type", ""},
           {"object_name", ""},
           {"code", issue.code},
           {"severity", "error"},
           {"message", issue.message},
           {"byte_start", nullptr},
           {"byte_end", nullptr},
           {"quality", "Unknown"},
           {"basis", "native catalog decode"}});
    }
  }
  auto object_schema = write_cli_report(request.output_directory, "inventory_objects", rows,
                                        "axklib inventory", request.overwrite);
  if (!object_schema)
    return report_failure(object_schema.error());
  auto issue_schema = write_cli_report(request.output_directory, "decode_issues", issues,
                                       "axklib inventory", request.overwrite);
  if (!issue_schema)
    return report_failure(issue_schema.error());
  axk::ReportValue::Object type_counts;
  for (const auto &[name, count] : counts)
    type_counts.emplace_back(name, count);
  axk::ReportValue::Array load_errors;
  for (const auto &row : loaded.errors)
    load_errors.emplace_back(axk::ReportValue::Object{row.begin(), row.end()});
  axk::ReportRow summary{
      {"input_count", static_cast<std::uint64_t>(loaded.loaded.size() + loaded.errors.size())},
      {"object_count", static_cast<std::uint64_t>(rows.size())},
      {"decode_issue_count", static_cast<std::uint64_t>(issues.size())},
      {"load_error_count", static_cast<std::uint64_t>(loaded.errors.size())},
      {"object_type_counts", std::move(type_counts)},
      {"load_errors", std::move(load_errors)}};
  if (auto written = axk::write_report_object(request.output_directory / "inventory_summary.json",
                                              summary, request.overwrite);
      !written)
    return report_failure(written.error());
  axk::ReportSchemaOptions summary_options;
  summary_options.source_command = "axklib";
  summary_options.library_version = std::string{axk::version()};
  auto summary_schema =
      axk::make_report_schema("inventory_summary", std::span{&summary, 1U}, summary_options);
  if (auto written = axk::write_report_schema(request.output_directory / "_schemas" /
                                                  "inventory_summary.schema.json",
                                              summary_schema, request.overwrite);
      !written)
    return report_failure(written.error());
  const std::array schemas{*object_schema, *issue_schema, summary_schema};
  if (auto index = axk::write_report_schema_index(
          request.output_directory / "_schemas" / "schema_index.json", schemas, request.overwrite);
      !index)
    return report_failure(index.error());
  std::cout << "objects=" << rows.size() << " decode_issues=" << issues.size()
            << " load_errors=" << loaded.errors.size() << '\n';
  std::cout << "reports written to " << axk::text::path_to_utf8(request.output_directory) << '\n';
  return loaded.errors.empty() ? 0 : 1;
}

std::vector<axk::ReportRow> relationship_rows(const CliLoadResult &loaded) {
  std::vector<axk::ReportRow> rows;
  for (const auto &source : loaded.loaded) {
    for (const auto &row : source.graph.relationships)
      rows.push_back(relationship_report_row(source, row));
  }
  return rows;
}

const axk::ObjectSnapshot *catalog_object(const CliLoaded &loaded, std::string_view key) {
  const auto found = std::ranges::find(loaded.catalog.objects, key, &axk::ObjectSnapshot::key);
  return found == loaded.catalog.objects.end() ? nullptr : &*found;
}

const axk::MediaObject *media_object(const CliLoaded &loaded, std::string_view key) {
  const auto found = std::ranges::find(loaded.objects, key, &axk::MediaObject::key);
  return found == loaded.objects.end() ? nullptr : &*found;
}

const axk::FatFile *fat_file_metadata(const CliLoaded &loaded, const axk::MediaObject *object) {
  if (object == nullptr)
    return nullptr;
  const auto *fat = std::get_if<axk::FatImage>(&loaded.media.storage());
  if (fat == nullptr)
    return nullptr;
  const auto found = std::ranges::find(fat->files(), object->logical_path, &axk::FatFile::path);
  return found == fat->files().end() ? nullptr : &*found;
}

std::uint64_t sfs_payload_offset(const CliLoaded &loaded, const axk::ObjectSnapshot &item) {
  if (loaded.media.kind() != axk::MediaKind::sfs)
    return 0U;
  const auto &container = std::get<axk::Container>(loaded.media.storage());
  const auto partition = std::ranges::find(container.partitions(), item.partition.value,
                                           [](const auto &row) { return row.index.value; });
  if (partition == container.partitions().end())
    return 0U;
  const auto record = std::ranges::find(partition->records, item.sfs_id.value,
                                        [](const auto &row) { return row.sfs_id.value; });
  if (record == partition->records.end() || record->extents.empty())
    return 0U;
  return (static_cast<std::uint64_t>(partition->start_sector) +
          static_cast<std::uint64_t>(record->extents.front().cluster_offset) *
              partition->sectors_per_cluster) *
         container.superblock().sector_size_bytes;
}

std::string joined_strings(const std::vector<std::string> &items) {
  std::string result;
  for (const auto &item : items) {
    if (!result.empty())
      result += '|';
    result += item;
  }
  return result;
}

std::string joined_programs(const std::vector<std::uint8_t> &items) {
  std::string result;
  for (const auto item : items) {
    if (!result.empty())
      result += '|';
    result += std::format("{:03}", item);
  }
  return result;
}

axk::ReportValue optional_unsigned(bool present, std::uint64_t value) {
  return present ? axk::ReportValue{value} : axk::ReportValue{nullptr};
}

std::vector<axk::ReportRow> sbac_detail_rows(const CliLoadResult &loaded) {
  std::vector<axk::ReportRow> rows;
  for (const auto &source : loaded.loaded) {
    for (const auto &relation : source.graph.relationships) {
      if (relation.type != "SBAC_SLOT_TO_SBNK")
        continue;
      const auto *sbac_item = catalog_object(source, relation.source_key);
      if (sbac_item == nullptr)
        continue;
      const auto *sbac = std::get_if<axk::CurrentSbac>(&sbac_item->object.payload);
      if (sbac == nullptr)
        continue;
      const auto matched =
          relation.target_key ? catalog_object(source, *relation.target_key) : nullptr;
      std::size_t slot_index{};
      const axk::SbacSlot *slot{};
      for (std::size_t index = 0; index < sbac->slots.size(); ++index) {
        if (sbac->slots[index].name == relation.assignment_name ||
            (relation.assignment_name.empty() && matched != nullptr &&
             sbac->slots[index].name == matched->object.header.name)) {
          slot_index = index;
          slot = &sbac->slots[index];
          break;
        }
      }
      if (slot == nullptr) {
        const auto named =
            std::ranges::find_if(sbac->slots, [](const auto &item) { return !item.name.empty(); });
        if (named == sbac->slots.end())
          continue;
        slot_index = static_cast<std::size_t>(std::distance(sbac->slots.begin(), named));
        slot = &*named;
      }
      std::vector<std::string> candidate_keys;
      std::vector<std::string> candidate_files;
      std::vector<std::string> candidate_names;
      for (const auto &key : relation.candidate_keys) {
        candidate_keys.push_back(public_object_key(source, key));
        if (const auto *candidate = catalog_object(source, key))
          candidate_names.push_back(candidate->object.header.name);
        if (const auto *object = media_object(source, key))
          candidate_files.push_back(object->logical_path);
      }
      const auto *sbac_media = media_object(source, sbac_item->key);
      const auto *matched_media = matched == nullptr ? nullptr : media_object(source, matched->key);
      const auto *sbac_fat = fat_file_metadata(source, sbac_media);
      const auto *matched_fat = fat_file_metadata(source, matched_media);
      const bool sfs = source.media.kind() == axk::MediaKind::sfs;
      const bool iso = source.media.kind() == axk::MediaKind::iso9660;
      const bool fat = source.media.kind() == axk::MediaKind::fat12_floppy;
      const auto match_notes = relation.quality == axk::RelationshipQuality::known
                                   ? "Input consistency: counted SBAC slot name uniquely matches a "
                                     "same-scope SBNK header name. The companion 32-bit slot word "
                                     "is preserved as raw/opaque."
                                   : relation.notes;
      rows.push_back({
          {"image", axk::text::path_to_utf8(source.path)},
          {"container_kind", media_kind_text(source.media.kind())},
          {"scope_key", public_scope_key(source, *sbac_item)},
          {"sbac_object_key", public_object_key(source, sbac_item->key)},
          {"sbac_partition_index", optional_unsigned(sfs, sbac_item->partition.value)},
          {"sbac_sfs_id", optional_unsigned(sfs, sbac_item->sfs_id.value)},
          {"sbac_fat_file", fat && sbac_media != nullptr ? sbac_media->logical_path : ""},
          {"sbac_payload_offset", optional_unsigned(sfs || sbac_media != nullptr,
                                                    sfs ? sfs_payload_offset(source, *sbac_item)
                                                        : sbac_media->data_offset)},
          {"sbac_name", sbac_item->object.header.name},
          {"sbac_payload_size", sbac_media != nullptr
                                    ? static_cast<std::uint64_t>(sbac_media->raw_payload.size())
                                    : std::uint64_t{0}},
          {"sbac_slot_count_0x144", static_cast<std::uint64_t>(sbac->active_slot_count)},
          {"slot_index", static_cast<std::uint64_t>(slot_index)},
          {"slot_offset", static_cast<std::uint64_t>(slot->offset)},
          {"slot_sbnk_name", slot->name},
          {"slot_raw_handle_0x10", static_cast<std::uint64_t>(slot->raw_handle)},
          {"match_method", relation.basis},
          {"match_quality", std::string{axk::relationship_quality_name(relation.quality)}},
          {"match_notes", match_notes},
          {"candidate_count", static_cast<std::uint64_t>(relation.candidate_keys.size())},
          {"candidate_object_keys", joined_strings(candidate_keys)},
          {"candidate_fat_files", fat ? joined_strings(candidate_files) : ""},
          {"candidate_names", joined_strings(candidate_names)},
          {"matched_sbnk_object_key",
           matched == nullptr ? "" : public_object_key(source, matched->key)},
          {"matched_sbnk_partition_index",
           optional_unsigned(sfs && matched != nullptr,
                             matched == nullptr ? 0U : matched->partition.value)},
          {"matched_sbnk_sfs_id",
           optional_unsigned(sfs && matched != nullptr,
                             matched == nullptr ? 0U : matched->sfs_id.value)},
          {"matched_sbnk_fat_file",
           fat && matched_media != nullptr ? matched_media->logical_path : ""},
          {"matched_sbnk_payload_offset",
           optional_unsigned(matched != nullptr && (sfs || matched_media != nullptr),
                             matched == nullptr ? 0U
                             : sfs              ? sfs_payload_offset(source, *matched)
                                                : matched_media->data_offset)},
          {"matched_sbnk_name", matched == nullptr ? "" : matched->object.header.name},
          {"notes", ""},
          {"sbac_iso_extent_sector",
           optional_unsigned(iso && sbac_media != nullptr,
                             sbac_media == nullptr ? 0U : sbac_media->data_offset / 2048U)},
          {"sbac_iso_data_offset",
           optional_unsigned(iso && sbac_media != nullptr,
                             sbac_media == nullptr ? 0U : sbac_media->data_offset)},
          {"sbac_iso_file_size", optional_unsigned(iso && sbac_media != nullptr,
                                                   sbac_media == nullptr ? 0U : sbac_media->size)},
          {"sbac_iso_recovery_quality", iso ? "clean-iso9660-object" : ""},
          {"sbac_fat_directory_offset",
           optional_unsigned(sbac_fat != nullptr,
                             sbac_fat == nullptr ? 0U : sbac_fat->directory_offset)},
          {"sbac_fat_first_cluster",
           optional_unsigned(sbac_fat != nullptr,
                             sbac_fat == nullptr ? 0U : sbac_fat->first_cluster)},
          {"sbac_fat_cluster_count",
           optional_unsigned(sbac_fat != nullptr,
                             sbac_fat == nullptr ? 0U : sbac_fat->clusters.size())},
          {"sbac_fat_file_size", optional_unsigned(fat && sbac_media != nullptr,
                                                   sbac_media == nullptr ? 0U : sbac_media->size)},
          {"sbac_fat_object_offset",
           optional_unsigned(fat && sbac_media != nullptr,
                             sbac_media == nullptr ? 0U : sbac_media->data_offset)},
          {"sbac_fat_stored_payload_offset",
           optional_unsigned(sbac_fat != nullptr,
                             sbac_fat == nullptr ? 0U : sbac_fat->first_data_offset)},
          {"matched_sbnk_iso_extent_sector",
           optional_unsigned(iso && matched_media != nullptr,
                             matched_media == nullptr ? 0U : matched_media->data_offset / 2048U)},
          {"matched_sbnk_iso_data_offset",
           optional_unsigned(iso && matched_media != nullptr,
                             matched_media == nullptr ? 0U : matched_media->data_offset)},
          {"matched_sbnk_iso_file_size",
           optional_unsigned(iso && matched_media != nullptr,
                             matched_media == nullptr ? 0U : matched_media->size)},
          {"matched_sbnk_iso_recovery_quality", iso ? "clean-iso9660-object" : ""},
          {"matched_sbnk_fat_directory_offset",
           optional_unsigned(matched_fat != nullptr,
                             matched_fat == nullptr ? 0U : matched_fat->directory_offset)},
          {"matched_sbnk_fat_first_cluster",
           optional_unsigned(matched_fat != nullptr,
                             matched_fat == nullptr ? 0U : matched_fat->first_cluster)},
          {"matched_sbnk_fat_cluster_count",
           optional_unsigned(matched_fat != nullptr,
                             matched_fat == nullptr ? 0U : matched_fat->clusters.size())},
          {"matched_sbnk_fat_file_size",
           optional_unsigned(fat && matched_media != nullptr,
                             matched_media == nullptr ? 0U : matched_media->size)},
          {"matched_sbnk_fat_object_offset",
           optional_unsigned(fat && matched_media != nullptr,
                             matched_media == nullptr ? 0U : matched_media->data_offset)},
          {"matched_sbnk_fat_stored_payload_offset",
           optional_unsigned(matched_fat != nullptr,
                             matched_fat == nullptr ? 0U : matched_fat->first_data_offset)},
      });
    }
  }
  return rows;
}

std::vector<axk::ReportRow> bitmap_detail_rows(const CliLoadResult &loaded) {
  static constexpr std::string_view notes =
      "Validated standalone assignment rows support SBNK+0x0c0..0x0cf as four big-endian "
      "program-link bitmap words for direct PROG->SBNK/sample assignments. PROG->SBAC "
      "assignments are reported separately as indirection and are not expected to set child "
      "SBNK bits.";
  std::vector<axk::ReportRow> rows;
  for (const auto &source : loaded.loaded) {
    for (const auto &comparison : source.graph.bitmap_comparisons) {
      const auto *item = catalog_object(source, comparison.sbnk_key);
      if (item == nullptr)
        continue;
      const auto *bank = std::get_if<axk::CurrentSbnk>(&item->object.payload);
      if (bank == nullptr)
        continue;
      const auto *object = media_object(source, item->key);
      const bool sfs = source.media.kind() == axk::MediaKind::sfs;
      const bool fat = source.media.kind() == axk::MediaKind::fat12_floppy;
      std::vector<std::string> direct_details;
      std::vector<std::string> ambiguous_programs;
      std::vector<std::string> ambiguous_details;
      for (const auto &relation : source.graph.relationships) {
        if (relation.type != "PROG_ASSIGNMENT_TO_SBNK" || !relation.assignment_index)
          continue;
        const auto direct = relation.target_key && *relation.target_key == item->key;
        const auto ambiguous =
            relation.quality == axk::RelationshipQuality::tentative &&
            std::ranges::find(relation.candidate_keys, item->key) != relation.candidate_keys.end();
        if (!direct && !ambiguous)
          continue;
        const auto *program_item = catalog_object(source, relation.source_key);
        if (program_item == nullptr)
          continue;
        const auto *program = std::get_if<axk::CurrentProg>(&program_item->object.payload);
        if (program == nullptr || *relation.assignment_index >= program->assignments.size())
          continue;
        const auto &assignment = program->assignments[*relation.assignment_index];
        const auto detail =
            std::format("{}@slot{}:kind0x{:02x}:flag0x{:02x}", program_item->object.header.name,
                        *relation.assignment_index, assignment.kind, assignment.flags);
        if (direct) {
          direct_details.push_back(detail);
        } else {
          ambiguous_programs.push_back(program_item->object.header.name);
          ambiguous_details.push_back(detail);
        }
      }
      std::ranges::sort(direct_details);
      std::ranges::sort(ambiguous_programs);
      std::ranges::sort(ambiguous_details);
      direct_details.erase(std::unique(direct_details.begin(), direct_details.end()),
                           direct_details.end());
      ambiguous_programs.erase(std::unique(ambiguous_programs.begin(), ambiguous_programs.end()),
                               ambiguous_programs.end());
      ambiguous_details.erase(std::unique(ambiguous_details.begin(), ambiguous_details.end()),
                              ambiguous_details.end());
      rows.push_back({
          {"image", axk::text::path_to_utf8(source.path)},
          {"container_kind", media_kind_text(source.media.kind())},
          {"scope_key", public_scope_key(source, *item)},
          {"sbnk_object_key", public_object_key(source, item->key)},
          {"sbnk_partition_index", optional_unsigned(sfs, item->partition.value)},
          {"sbnk_sfs_id", optional_unsigned(sfs, item->sfs_id.value)},
          {"sbnk_fat_file", fat && object != nullptr ? object->logical_path : ""},
          {"sbnk_payload_offset",
           optional_unsigned(sfs || object != nullptr,
                             sfs ? sfs_payload_offset(source, *item) : object->data_offset)},
          {"sbnk_name", item->object.header.name},
          {"linked_programs_001_032_bitmap_0x0c0",
           static_cast<std::uint64_t>(bank->linked_program_bitmap_words[0])},
          {"linked_programs_033_064_bitmap_0x0c4",
           static_cast<std::uint64_t>(bank->linked_program_bitmap_words[1])},
          {"linked_programs_065_096_bitmap_0x0c8",
           static_cast<std::uint64_t>(bank->linked_program_bitmap_words[2])},
          {"linked_programs_097_128_bitmap_0x0cc",
           static_cast<std::uint64_t>(bank->linked_program_bitmap_words[3])},
          {"bitmap_programs", joined_programs(comparison.bitmap_programs)},
          {"direct_prog_assignment_programs",
           joined_programs(comparison.direct_assignment_programs)},
          {"direct_prog_assignment_details", joined_strings(direct_details)},
          {"ambiguous_direct_assignment_programs", joined_strings(ambiguous_programs)},
          {"ambiguous_direct_assignment_details", joined_strings(ambiguous_details)},
          {"sbac_indirect_assignment_programs",
           joined_programs(comparison.indirect_assignment_programs)},
          {"bitmap_without_direct_assignment_programs",
           joined_programs(comparison.bitmap_without_direct)},
          {"direct_assignment_without_bitmap_programs",
           joined_programs(comparison.direct_without_bitmap)},
          {"mismatch_class", comparison.mismatch_class},
          {"match_status", comparison.status},
          {"quality", comparison.status == "match" ? "Known" : "Tentative"},
          {"notes", std::string{notes}},
      });
    }
  }
  return rows;
}

std::vector<axk::ReportRow> program_detail_rows(const CliLoadResult &loaded) {
  std::vector<axk::ReportRow> rows;
  for (const auto &source : loaded.loaded) {
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
      const auto *target =
          relation.target_key ? catalog_object(source, *relation.target_key) : nullptr;
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
          candidate_categories.push_back(object_type_text(candidate->object.header.type));
        }
        if (const auto *object = media_object(source, key))
          candidate_files.push_back(object->logical_path);
      }
      std::ranges::sort(candidate_categories);
      candidate_categories.erase(
          std::unique(candidate_categories.begin(), candidate_categories.end()),
          candidate_categories.end());
      axk::ReportValue child_count{nullptr};
      if (target != nullptr && target->object.header.type == axk::ObjectType::sbac) {
        child_count = static_cast<std::uint64_t>(
            std::ranges::count_if(source.graph.relationships, [&](const auto &row) {
              return row.source_key == target->key && row.type == "SBAC_SLOT_TO_SBNK" &&
                     row.target_key.has_value();
            }));
      }
      const auto expected = assignment.kind == 0x11U   ? "SBAC"
                            : assignment.kind == 0x10U ? "SBNK"
                                                       : "";
      rows.push_back({
          {"image", axk::text::path_to_utf8(source.path)},
          {"container_kind", media_kind_text(source.media.kind())},
          {"scope_key", public_scope_key(source, *program_item)},
          {"prog_object_key", public_object_key(source, program_item->key)},
          {"prog_partition_index", optional_unsigned(sfs, program_item->partition.value)},
          {"prog_sfs_id", optional_unsigned(sfs, program_item->sfs_id.value)},
          {"prog_fat_file", fat && program_media != nullptr ? program_media->logical_path : ""},
          {"prog_payload_offset", optional_unsigned(sfs || program_media != nullptr,
                                                    sfs ? sfs_payload_offset(source, *program_item)
                                                        : program_media->data_offset)},
          {"prog_name", program_item->object.header.name},
          {"prog_payload_size", program_media != nullptr
                                    ? static_cast<std::uint64_t>(program_media->raw_payload.size())
                                    : std::uint64_t{0}},
          {"assignment_index", static_cast<std::uint64_t>(*relation.assignment_index)},
          {"assignment_offset",
           static_cast<std::uint64_t>(0x120U + *relation.assignment_index * 0x38U)},
          {"assignment_name", assignment.name},
          {"assignment_raw_handle_0x10", static_cast<std::uint64_t>(assignment.raw_handle)},
          {"assignment_kind_byte_0x14", static_cast<std::uint64_t>(assignment.kind)},
          {"assignment_flag_byte_0x15", static_cast<std::uint64_t>(assignment.flags)},
          {"assignment_output1_byte_0x1d",
           static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(assignment.raw_row[0x1d]))},
          {"assignment_rch_assign_gate_byte_0x28",
           static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(assignment.raw_row[0x28]))},
          {"assignment_rch_assign_display", relation.receive_channel_display},
          {"selector_expected_category", expected},
          {"assignment_row_state", "decoded-row"},
          {"active_assignment_state",
           std::string{axk::assignment_state_name(relation.assignment_state)}},
          {"match_method", relation.basis},
          {"match_quality", std::string{axk::relationship_quality_name(relation.quality)}},
          {"match_notes", relation.notes},
          {"candidate_count", static_cast<std::uint64_t>(relation.candidate_keys.size())},
          {"candidate_categories", joined_strings(candidate_categories)},
          {"candidate_object_keys", joined_strings(candidate_keys)},
          {"candidate_fat_files", fat ? joined_strings(candidate_files) : ""},
          {"candidate_names", joined_strings(candidate_names)},
          {"matched_target_type",
           target == nullptr ? "" : object_type_text(target->object.header.type)},
          {"matched_target_object_key",
           target == nullptr ? "" : public_object_key(source, target->key)},
          {"matched_target_partition_index",
           optional_unsigned(sfs && target != nullptr,
                             target == nullptr ? 0U : target->partition.value)},
          {"matched_target_sfs_id",
           optional_unsigned(sfs && target != nullptr,
                             target == nullptr ? 0U : target->sfs_id.value)},
          {"matched_target_fat_file",
           fat && target_media != nullptr ? target_media->logical_path : ""},
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
          {"prog_iso_data_offset",
           optional_unsigned(iso && program_media != nullptr,
                             program_media == nullptr ? 0U : program_media->data_offset)},
          {"prog_iso_file_size",
           optional_unsigned(iso && program_media != nullptr,
                             program_media == nullptr ? 0U : program_media->size)},
          {"prog_iso_recovery_quality", iso ? "clean-iso9660-object" : ""},
          {"prog_fat_directory_offset",
           optional_unsigned(program_fat != nullptr,
                             program_fat == nullptr ? 0U : program_fat->directory_offset)},
          {"prog_fat_first_cluster",
           optional_unsigned(program_fat != nullptr,
                             program_fat == nullptr ? 0U : program_fat->first_cluster)},
          {"prog_fat_cluster_count",
           optional_unsigned(program_fat != nullptr,
                             program_fat == nullptr ? 0U : program_fat->clusters.size())},
          {"prog_fat_file_size",
           optional_unsigned(fat && program_media != nullptr,
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
           optional_unsigned(iso && target_media != nullptr,
                             target_media == nullptr ? 0U : target_media->size)},
          {"matched_target_iso_recovery_quality",
           iso && target_media != nullptr ? "clean-iso9660-object" : ""},
          {"matched_target_fat_directory_offset",
           optional_unsigned(target_fat != nullptr,
                             target_fat == nullptr ? 0U : target_fat->directory_offset)},
          {"matched_target_fat_first_cluster",
           optional_unsigned(target_fat != nullptr,
                             target_fat == nullptr ? 0U : target_fat->first_cluster)},
          {"matched_target_fat_cluster_count",
           optional_unsigned(target_fat != nullptr,
                             target_fat == nullptr ? 0U : target_fat->clusters.size())},
          {"matched_target_fat_file_size",
           optional_unsigned(fat && target_media != nullptr,
                             target_media == nullptr ? 0U : target_media->size)},
          {"matched_target_fat_object_offset",
           optional_unsigned(fat && target_media != nullptr,
                             target_media == nullptr ? 0U : target_media->data_offset)},
          {"matched_target_fat_stored_payload_offset",
           optional_unsigned(target_fat != nullptr,
                             target_fat == nullptr ? 0U : target_fat->first_data_offset)},
      });
    }
  }
  return rows;
}

std::vector<axk::ReportRow> program_ignored_detail_rows(const CliLoadResult &loaded) {
  std::vector<axk::ReportRow> rows;
  for (const auto &source : loaded.loaded) {
    for (const auto &item : source.catalog.objects) {
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
        const bool name_match =
            std::ranges::any_of(source.catalog.objects, [&](const auto &target) {
              return target.scope_key == item.scope_key &&
                     target.object.header.name == assignment.name;
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
            {"image", axk::text::path_to_utf8(source.path)},
            {"container_kind", media_kind_text(source.media.kind())},
            {"scope_key", public_scope_key(source, item)},
            {"prog_object_key", public_object_key(source, item.key)},
            {"prog_partition_index", optional_unsigned(sfs, item.partition.value)},
            {"prog_sfs_id", optional_unsigned(sfs, item.sfs_id.value)},
            {"prog_fat_file", fat && program_media != nullptr ? program_media->logical_path : ""},
            {"prog_payload_offset",
             optional_unsigned(sfs || program_media != nullptr,
                               sfs                        ? sfs_payload_offset(source, item)
                               : program_media == nullptr ? 0U
                                                          : program_media->data_offset)},
            {"prog_name", item.object.header.name},
            {"prog_payload_size",
             program_media == nullptr
                 ? std::uint64_t{0}
                 : static_cast<std::uint64_t>(program_media->raw_payload.size())},
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

int run_orphans_request(const axk::cli::OrphansRequest &request) {
  if (const auto ready = prepare_report_directory(request.output_directory, request.overwrite);
      !ready)
    return report_failure(ready.error());
  std::vector<axk::ReportRow> rows;
  std::vector<axk::ReportRow> summaries;
  for (const auto &path : expand_cli_paths(request.paths)) {
    auto snapshot = load_semantic_snapshot(path);
    if (!snapshot) {
      std::cerr << axk::render_error(snapshot.error()) << '\n';
      return 2;
    }
    const auto report =
        axk::analyze_waveform_orphans(snapshot->container, snapshot->catalog, snapshot->graph);
    for (const auto &row : report.rows) {
      rows.push_back({
          {"source_path", axk::text::path_to_utf8(path)},
          {"partition_index", static_cast<std::uint64_t>(row.partition.value)},
          {"partition_name", row.partition_name},
          {"volume_name", row.volume_name},
          {"waveform_name", row.waveform_name},
          {"object_key", row.object_key},
          {"sfs_id", static_cast<std::uint64_t>(row.sfs_id.value)},
          {"smpl_link_id", static_cast<std::uint64_t>(row.smpl_link_id)},
          {"status", std::string{axk::waveform_status_name(row.status)}},
          {"referencing_sample_banks", joined_strings(row.referencing_sample_banks)},
          {"basis", row.basis},
          {"notes", row.notes},
      });
    }
    summaries.push_back({
        {"source_path", axk::text::path_to_utf8(path)},
        {"waveform_count", static_cast<std::uint64_t>(report.rows.size())},
        {"referenced_count", static_cast<std::uint64_t>(report.referenced_count)},
        {"known_unreferenced_count", static_cast<std::uint64_t>(report.known_unreferenced_count)},
        {"ambiguous_or_unresolved_count",
         static_cast<std::uint64_t>(report.ambiguous_or_unresolved_count)},
    });
  }
  auto row_schema = write_cli_report(request.output_directory, "waveform_orphans", rows, "axklib",
                                     request.overwrite);
  if (!row_schema)
    return report_failure(row_schema.error());
  auto summary_schema = write_cli_report(request.output_directory, "waveform_orphan_summary",
                                         summaries, "axklib", request.overwrite);
  if (!summary_schema)
    return report_failure(summary_schema.error());
  const std::array schemas{*row_schema, *summary_schema};
  if (auto index = axk::write_report_schema_index(
          request.output_directory / "_schemas" / "schema_index.json", schemas, request.overwrite);
      !index)
    return report_failure(index.error());
  for (const auto &summary : summaries) {
    const auto text = [&](std::string_view name) {
      const auto found =
          std::ranges::find(summary, name, &std::pair<std::string, axk::ReportValue>::first);
      if (found == summary.end())
        return std::string{};
      if (const auto *value = std::get_if<std::string>(&found->second.value))
        return *value;
      if (const auto *value = std::get_if<std::uint64_t>(&found->second.value))
        return std::to_string(*value);
      return std::string{};
    };
    std::cout << "image=" << text("source_path") << " waveforms=" << text("waveform_count")
              << " referenced=" << text("referenced_count")
              << " known_unreferenced=" << text("known_unreferenced_count")
              << " ambiguous_or_unresolved=" << text("ambiguous_or_unresolved_count") << '\n';
  }
  std::cout << "reports written to " << axk::text::path_to_utf8(request.output_directory) << '\n';
  return 0;
}

std::uint64_t mismatch_cluster_count(std::span<const axk::AllocationMismatchRange> ranges) {
  std::uint64_t result{};
  for (const auto &range : ranges)
    result += static_cast<std::uint64_t>(range.end_cluster) - range.start_cluster + 1U;
  return result;
}

std::vector<axk::ReportRow> allocation_summary_rows(const std::filesystem::path &path,
                                                    const axk::Container &container) {
  std::vector<axk::ReportRow> rows;
  for (const auto &partition : container.partitions()) {
    const auto cluster_size = static_cast<std::uint64_t>(partition.sectors_per_cluster) *
                              container.superblock().sector_size_bytes;
    std::uint64_t direct_records{};
    std::uint64_t continuation_records{};
    std::uint64_t extent_count{};
    std::uint64_t continuation_clusters{};
    std::uint64_t first_payload = partition.cluster_count;
    std::uint64_t first_object = partition.cluster_count;
    for (const auto &record : partition.records) {
      if (record.continuation_clusters.empty())
        ++direct_records;
      else
        ++continuation_records;
      extent_count += record.extents.size();
      continuation_clusters += record.continuation_clusters.size();
      for (const auto &extent : record.extents)
        first_payload = std::min(first_payload, static_cast<std::uint64_t>(extent.cluster_offset));
      if (record.payload_kind == axk::PayloadKind::object ||
          record.payload_kind == axk::PayloadKind::alternating_byte_object) {
        for (const auto &extent : record.extents)
          first_object = std::min(first_object, static_cast<std::uint64_t>(extent.cluster_offset));
      }
    }
    std::string warnings;
    const auto &allocation = partition.allocation;
    const auto free = allocation.free_space;
    rows.push_back({
        {"source_image", axk::text::path_to_utf8(path)},
        {"partition_index", static_cast<std::uint64_t>(partition.index.value)},
        {"partition_name", partition.name},
        {"start_sector", static_cast<std::uint64_t>(partition.start_sector)},
        {"sectors_per_cluster", static_cast<std::uint64_t>(partition.sectors_per_cluster)},
        {"cluster_count", static_cast<std::uint64_t>(partition.cluster_count)},
        {"bitmap_offset",
         (static_cast<std::uint64_t>(partition.start_sector) +
          static_cast<std::uint64_t>(partition.bitmap_cluster) * partition.sectors_per_cluster) *
             container.superblock().sector_size_bytes},
        {"index_offset", (static_cast<std::uint64_t>(partition.start_sector) +
                          static_cast<std::uint64_t>(partition.directory_index_cluster) *
                              partition.sectors_per_cluster) *
                             container.superblock().sector_size_bytes},
        {"scanned_index_bytes", (first_object - partition.directory_index_cluster) * cluster_size},
        {"valid_index_record_count", static_cast<std::uint64_t>(partition.records.size())},
        {"invalid_extent_record_count",
         static_cast<std::uint64_t>(allocation.invalid_extent_record_count)},
        {"direct_extent_record_count", direct_records},
        {"continuation_extent_record_count", continuation_records},
        {"data_extent_count", extent_count},
        {"continuation_list_cluster_count", continuation_clusters},
        {"stored_used_cluster_count",
         static_cast<std::uint64_t>(allocation.stored_used_cluster_count)},
        {"reconstructed_used_cluster_count",
         static_cast<std::uint64_t>(allocation.reconstructed_used_cluster_count)},
        {"first_payload_cluster", first_payload},
        {"reserved_cluster_count",
         free ? static_cast<std::uint64_t>(free->reserved_cluster_count) : first_payload},
        {"sampler_free_cluster_count",
         free ? static_cast<std::uint64_t>(free->free_cluster_count) : std::uint64_t{0}},
        {"sampler_free_bytes", free ? free->free_bytes : std::uint64_t{0}},
        {"sampler_visible_free_kib", free ? free->sampler_visible_free_kib : std::uint64_t{0}},
        {"stored_used_not_reconstructed_count",
         mismatch_cluster_count(allocation.stored_not_reconstructed)},
        {"reconstructed_used_not_stored_count",
         mismatch_cluster_count(allocation.reconstructed_not_stored)},
        {"extent_total_mismatch_count",
         static_cast<std::uint64_t>(allocation.extent_total_mismatch_count)},
        {"warning_count", std::uint64_t{0}},
        {"warnings", warnings},
    });
  }
  return rows;
}

std::vector<axk::ReportRow> allocation_extent_rows(const std::filesystem::path &path,
                                                   const axk::Container &container) {
  std::vector<axk::ReportRow> rows;
  for (const auto &partition : container.partitions()) {
    for (const auto &record : partition.records) {
      for (std::size_t index = 0; index < record.extents.size(); ++index) {
        const auto &extent = record.extents[index];
        rows.push_back({
            {"source_image", axk::text::path_to_utf8(path)},
            {"partition_index", static_cast<std::uint64_t>(partition.index.value)},
            {"sfs_id", static_cast<std::uint64_t>(record.sfs_id.value)},
            {"record_offset", record.record_offset.value},
            {"extent_kind", "data"},
            {"extent_index", static_cast<std::uint64_t>(index)},
            {"cluster_offset", static_cast<std::uint64_t>(extent.cluster_offset)},
            {"cluster_count", static_cast<std::uint64_t>(extent.cluster_count)},
            {"byte_count", static_cast<std::uint64_t>(extent.byte_count)},
            {"continuation_cluster", nullptr},
        });
      }
    }
  }
  return rows;
}

std::vector<axk::ReportRow> allocation_mismatch_rows(const std::filesystem::path &path,
                                                     std::span<const axk::Partition> partitions) {
  std::vector<axk::ReportRow> rows;
  const auto append = [&](const axk::Partition &partition, std::string direction,
                          std::span<const axk::AllocationMismatchRange> ranges) {
    for (const auto &range : ranges) {
      rows.push_back({
          {"source_image", axk::text::path_to_utf8(path)},
          {"partition_index", static_cast<std::uint64_t>(partition.index.value)},
          {"partition_name", partition.name},
          {"direction", direction},
          {"start_cluster", static_cast<std::uint64_t>(range.start_cluster)},
          {"end_cluster", static_cast<std::uint64_t>(range.end_cluster)},
          {"cluster_count",
           static_cast<std::uint64_t>(range.end_cluster) - range.start_cluster + 1U},
      });
    }
  };
  for (const auto &partition : partitions) {
    append(partition, "stored-used-without-index-extent",
           partition.allocation.stored_not_reconstructed);
    append(partition, "index-extent-references-free-cluster",
           partition.allocation.reconstructed_not_stored);
  }
  return rows;
}

std::vector<axk::ReportRow> volume_validation_rows(const std::filesystem::path &path,
                                                   const axk::Container &container,
                                                   const axk::ObjectCatalog &catalog,
                                                   std::vector<axk::ReportRow> &detail_issues,
                                                   std::vector<axk::ReportRow> &validation_issues) {
  using VolumeKey = std::tuple<std::uint8_t, std::uint32_t, std::string, std::string>;
  std::map<VolumeKey, std::vector<const axk::ObjectSnapshot *>> volumes;
  for (const auto &partition : container.partitions()) {
    std::map<std::uint32_t, const axk::IndexRecord *> directories;
    for (const auto &record : partition.records) {
      if (record.directory_id)
        directories.emplace(record.directory_id->value, &record);
    }
    const axk::IndexRecord *root{};
    for (const auto &[id, directory] : directories) {
      if (directory->parent_directory_id && directory->parent_directory_id->value == id) {
        root = directory;
        break;
      }
    }
    if (root == nullptr || !root->directory_id)
      continue;
    for (const auto &entry : root->directory_entries) {
      const auto found = directories.find(entry.link_id.value);
      if (entry.name == "." || entry.name == ".." || found == directories.end())
        continue;
      const auto *volume = found->second;
      if (!volume->parent_directory_id ||
          volume->parent_directory_id->value != root->directory_id->value)
        continue;
      volumes.try_emplace(
          VolumeKey{partition.index.value, volume->sfs_id.value, partition.name, entry.name});
    }
  }
  for (const auto &item : catalog.objects) {
    if (item.placement) {
      volumes[{item.partition.value, item.placement->volume_directory.value,
               item.placement->partition_name, item.placement->volume_name}]
          .push_back(&item);
    }
  }
  std::vector<axk::ReportRow> rows;
  for (const auto &[key, objects] : volumes) {
    static_cast<void>(objects);
    const auto &[partition_index, directory_id, partition_name, volume_name] = key;
    const auto partition = std::ranges::find(container.partitions(), partition_index,
                                             [](const auto &item) { return item.index.value; });
    const axk::IndexRecord *volume_record{};
    if (partition != container.partitions().end()) {
      const auto found = std::ranges::find_if(partition->records, [&](const auto &record) {
        return record.sfs_id.value == directory_id;
      });
      if (found != partition->records.end())
        volume_record = &*found;
    }
    std::uint64_t category_count{};
    std::uint64_t object_entry_count{};
    std::uint64_t matched_object_count{};
    std::uint64_t category_directory_count{};
    std::uint64_t checked_entry_count{};
    std::uint64_t valid_entry_count{};
    std::uint64_t current_object_count{};
    std::map<axk::ObjectType, std::uint64_t> artifact_counts;
    if (partition != container.partitions().end() && volume_record != nullptr) {
      const auto category_type = [](std::string_view name) {
        if (name == "SMPL")
          return axk::ObjectType::smpl;
        if (name == "SBNK")
          return axk::ObjectType::sbnk;
        if (name == "SBAC")
          return axk::ObjectType::sbac;
        if (name == "PROG")
          return axk::ObjectType::prog;
        if (name == "SEQU")
          return axk::ObjectType::sequ;
        return axk::ObjectType::unknown;
      };
      for (const auto &category_entry : volume_record->directory_entries) {
        if (category_entry.name == "." || category_entry.name == "..")
          continue;
        ++category_count;
        const auto type = category_type(category_entry.name);
        if (type == axk::ObjectType::unknown)
          continue;
        const auto category =
            std::ranges::find(partition->records, category_entry.link_id.value,
                              [](const auto &record) { return record.sfs_id.value; });
        if (category == partition->records.end())
          continue;
        ++category_directory_count;
        for (const auto &entry : category->directory_entries) {
          if (entry.name == "." || entry.name == "..")
            continue;
          ++object_entry_count;
          ++checked_entry_count;
          const auto target =
              std::ranges::find(partition->records, entry.link_id.value,
                                [](const auto &record) { return record.sfs_id.value; });
          if (target == partition->records.end() ||
              (target->payload_kind != axk::PayloadKind::object &&
               target->payload_kind != axk::PayloadKind::alternating_byte_object))
            continue;
          ++matched_object_count;
          ++valid_entry_count;
          if (target->payload_kind == axk::PayloadKind::alternating_byte_object)
            ++artifact_counts[type];
          else
            ++current_object_count;
        }
      }
    }
    const auto allocation_issues = partition == container.partitions().end()
                                       ? 1U
                                       : partition->allocation.invalid_extent_record_count +
                                             partition->allocation.extent_total_mismatch_count;
    std::uint64_t artifact_count{};
    for (const auto &[type, count] : artifact_counts) {
      static_cast<void>(type);
      artifact_count += count;
    }
    const auto artifact_smpl_count = artifact_counts[axk::ObjectType::smpl];
    const auto warning_count = artifact_count == 0U ? 0U : 1U;
    const auto details = std::format(
        "visible alternating-byte compatibility artifact object entries: total={}, SMPL={}, "
        "SBNK={}, SBAC={}, PROG={}; filesystem tree/allocation validation does not prove sampler "
        "loadability for this physical alternating-byte artifact family",
        artifact_count, artifact_smpl_count, artifact_counts[axk::ObjectType::sbnk],
        artifact_counts[axk::ObjectType::sbac], artifact_counts[axk::ObjectType::prog]);
    if (artifact_count != 0U) {
      detail_issues.push_back({
          {"source_image", axk::text::path_to_utf8(path)},
          {"partition_index", static_cast<std::uint64_t>(partition_index)},
          {"partition_name", partition_name},
          {"volume_name", volume_name},
          {"volume_path", "/" + volume_name},
          {"severity", "warning"},
          {"issue_type", "visible-alternating-byte-compatibility-artifact-objects"},
          {"category_code", ""},
          {"category_name", ""},
          {"category_directory_id", ""},
          {"category_directory_path", ""},
          {"entry_offset", ""},
          {"entry_name", ""},
          {"link_id", ""},
          {"target_kind", "object"},
          {"target_sfs_id", ""},
          {"target_payload_kind", "alternating-byte-compatibility-object"},
          {"match_quality", "Likely"},
          {"unmatched_reason", ""},
          {"details", details},
      });
      validation_issues.push_back({
          {"severity", "warning"},
          {"code", "SFS_VOLUME_VISIBLE_ALTERNATING_BYTE_ARTIFACT"},
          {"message", details},
          {"scope", "volume"},
          {"source_path", axk::text::path_to_utf8(path)},
          {"sampler_path", "/" + volume_name},
          {"object_key", ""},
          {"quality", "Likely"},
          {"basis", "axklib.validation.volume"},
          {"recommended_next_check", ""},
      });
    }
    const auto validation_status = allocation_issues != 0U ? "Fail"
                                   : warning_count != 0U   ? "Warn"
                                                           : "Pass";
    const auto classification = allocation_issues != 0U ? "volume-likely-corrupt"
                                : warning_count != 0U
                                    ? "valid-visible-tree-with-warnings"
                                    : "valid-visible-tree-hidden-unreferenced-not-an-error";
    rows.push_back({
        {"source_image", axk::text::path_to_utf8(path)},
        {"partition_index", static_cast<std::uint64_t>(partition_index)},
        {"partition_name", partition_name},
        {"volume_name", volume_name},
        {"volume_path", "/" + volume_name},
        {"directory_id", static_cast<std::uint64_t>(directory_id)},
        {"category_count", category_count},
        {"object_entry_count", object_entry_count},
        {"matched_object_count", matched_object_count},
        {"category_directory_count", category_directory_count},
        {"checked_category_entry_count", checked_entry_count},
        {"valid_category_entry_count", valid_entry_count},
        {"malformed_category_entry_count", std::uint64_t{0}},
        {"category_count_mismatch_count", std::uint64_t{0}},
        {"current_object_entry_count", current_object_count},
        {"compatibility_artifact_object_entry_count", artifact_count},
        {"compatibility_artifact_smpl_entry_count", artifact_smpl_count},
        {"fatal_issue_count", std::uint64_t{0}},
        {"warning_issue_count", static_cast<std::uint64_t>(warning_count)},
        {"allocation_status", allocation_issues == 0U ? "Pass" : "Fail"},
        {"allocation_issue_count", static_cast<std::uint64_t>(allocation_issues)},
        {"validation_status", validation_status},
        {"volume_classification", classification},
        {"quality_summary", warning_count != 0U ? details
                            : allocation_issues == 0U
                                ? "category directory entries and optional allocation check passed"
                                : "allocation check failed"},
    });
  }
  return rows;
}

axk::ReportRow export_validation_issue(std::string severity, std::string code, std::string message,
                                       std::string scope, const std::filesystem::path &source,
                                       std::string object_key = {}) {
  return {{"severity", std::move(severity)},
          {"code", std::move(code)},
          {"message", std::move(message)},
          {"scope", std::move(scope)},
          {"source_path", axk::text::path_to_utf8(source)},
          {"sampler_path", ""},
          {"object_key", std::move(object_key)},
          {"quality", "Known"},
          {"basis", "validation"},
          {"recommended_next_check", ""}};
}

std::optional<std::uint32_t> little_u32(std::span<const std::byte> bytes, std::size_t offset) {
  if (offset + 4U > bytes.size())
    return std::nullopt;
  return std::to_integer<std::uint8_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U])) << 8U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2U])) << 16U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3U])) << 24U);
}

std::optional<std::uint16_t> little_u16(std::span<const std::byte> bytes, std::size_t offset) {
  if (offset + 2U > bytes.size())
    return std::nullopt;
  return static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(bytes[offset]) |
      (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U])) << 8U));
}

std::vector<axk::ReportRow> validate_export_directory(const std::filesystem::path &root) {
  std::vector<axk::ReportRow> issues;
  if (!std::filesystem::exists(root)) {
    issues.push_back(export_validation_issue("error", "EXPORT_DIR_NOT_FOUND",
                                             "Export directory does not exist.", "export", root));
    return issues;
  }
  std::error_code iteration_error;
  for (std::filesystem::recursive_directory_iterator iterator{root, iteration_error}, end;
       iterator != end && !iteration_error; iterator.increment(iteration_error)) {
    if (!iterator->is_regular_file() || iterator->path().extension() != ".json" ||
        iterator->path().filename() == "schema_index.json" ||
        std::ranges::find(iterator->path(), "_schemas") != iterator->path().end())
      continue;
    Json record;
    try {
      std::ifstream input{iterator->path()};
      input >> record;
    } catch (const std::exception &error) {
      issues.push_back(
          export_validation_issue("error", "EXPORT_SIDECAR_BAD_JSON",
                                  std::string{"Sidecar JSON could not be parsed: "} + error.what(),
                                  "sidecar", iterator->path()));
      continue;
    }
    if (!record.is_object())
      continue;
    const auto schema = record.value("schema", std::string{});
    if (schema == "axklib.volume_graph.v1") {
      const auto inspect_path = [&](const Json &value, std::string_view object_key) {
        if (!value.is_string())
          return;
        const std::filesystem::path path{value.get<std::string>()};
        if (path.is_absolute() || std::ranges::find(path, "..") != path.end()) {
          issues.push_back(export_validation_issue(
              "error", "EXPORT_VOLUME_GRAPH_PATH_ESCAPE",
              "Volume graph WAV path must be relative and stay inside the export root.", "sidecar",
              iterator->path(), std::string{object_key}));
        }
      };
      if (record.contains("objects") && record["objects"].is_object() &&
          record["objects"].contains("smpl") && record["objects"]["smpl"].is_array()) {
        for (const auto &sample : record["objects"]["smpl"])
          inspect_path(sample.value("wav_path", Json{}), sample.value("object_key", std::string{}));
      }
      continue;
    }
    auto object_key = record.value("object_key", std::string{});
    Json header = record;
    std::filesystem::path wav_path;
    if (schema == "axklib.wave_sidecar.v2") {
      static constexpr std::array sections{"identity",   "audio",      "playback", "relationships",
                                           "parameters", "conversion", "origin"};
      std::vector<std::string> missing;
      for (const auto section : sections) {
        if (!record.contains(section))
          missing.emplace_back(section);
      }
      if (record.contains("identity") && record["identity"].is_object())
        object_key = record["identity"].value("object_key", object_key);
      if (!missing.empty()) {
        std::string section_names;
        for (const auto &section : missing) {
          if (!section_names.empty())
            section_names += ", ";
          section_names += section;
        }
        issues.push_back(
            export_validation_issue("error", "EXPORT_SIDECAR_MISSING_FIELD",
                                    "Sidecar missing required sections: " + section_names,
                                    "sidecar", iterator->path(), object_key));
      }
      if (!record.contains("audio") || !record["audio"].is_object())
        continue;
      header = record["audio"];
      wav_path = header.value("wav_path", std::string{});
      if (wav_path.is_absolute() || std::ranges::find(wav_path, "..") != wav_path.end()) {
        issues.push_back(export_validation_issue(
            "error", "EXPORT_SIDECAR_PATH_ESCAPE",
            "v2 sidecar audio.wav_path must be relative and stay inside the export root.",
            "sidecar", iterator->path(), object_key));
        continue;
      }
      wav_path = root / wav_path;
    } else {
      if (!record.contains("wav_path"))
        continue;
      static constexpr std::array required{
          "source_container",   "object_key",         "wav_path",     "sample_rate",
          "channels",           "sample_width_bytes", "frames",       "stored_payload_size",
          "extraction_quality", "extraction_basis",   "field_quality"};
      std::vector<std::string> missing;
      for (const auto field : required) {
        if (!record.contains(field))
          missing.emplace_back(field);
      }
      std::ranges::sort(missing);
      if (!missing.empty()) {
        std::string fields;
        for (const auto &field : missing) {
          if (!fields.empty())
            fields += ", ";
          fields += field;
        }
        issues.push_back(export_validation_issue("error", "EXPORT_SIDECAR_MISSING_FIELD",
                                                 "Sidecar missing required fields: " + fields,
                                                 "sidecar", iterator->path(), object_key));
      }
      wav_path = record.value("wav_path", std::string{});
      if (!wav_path.is_absolute()) {
        if (!std::filesystem::exists(wav_path))
          wav_path = iterator->path().parent_path() / wav_path;
      }
    }
    if (!std::filesystem::exists(wav_path)) {
      issues.push_back(export_validation_issue("error", "EXPORT_WAV_MISSING",
                                               "Referenced WAV does not exist: " +
                                                   axk::text::path_to_utf8(wav_path),
                                               "export", iterator->path(), object_key));
      continue;
    }
    std::ifstream wav{wav_path, std::ios::binary};
    std::array<std::byte, 44> bytes{};
    wav.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (wav.gcount() != static_cast<std::streamsize>(bytes.size()) ||
        std::string_view{reinterpret_cast<const char *>(bytes.data()), 4U} != "RIFF" ||
        std::string_view{reinterpret_cast<const char *>(bytes.data() + 8U), 4U} != "WAVE") {
      issues.push_back(
          export_validation_issue("error", "EXPORT_WAV_BAD_HEADER",
                                  "Referenced WAV could not be opened: invalid WAVE header",
                                  "export", wav_path, object_key));
      continue;
    }
    const std::array observed{
        std::pair{"sample_rate", static_cast<std::uint64_t>(*little_u32(bytes, 24U))},
        std::pair{"channels", static_cast<std::uint64_t>(*little_u16(bytes, 22U))},
        std::pair{"sample_width_bytes", static_cast<std::uint64_t>(*little_u16(bytes, 34U) / 8U)},
        std::pair{"frames", static_cast<std::uint64_t>(
                                *little_u32(bytes, 40U) /
                                (*little_u16(bytes, 22U) * (*little_u16(bytes, 34U) / 8U)))},
    };
    for (const auto &[name, value] : observed) {
      if (header.contains(name) && header[name].is_number_integer() &&
          header[name].get<std::uint64_t>() != value) {
        issues.push_back(export_validation_issue(
            "error", "EXPORT_WAV_HEADER_MISMATCH",
            std::format("{} sidecar={} wav={}", name, header[name].get<std::uint64_t>(), value),
            "export", wav_path, object_key));
      }
    }
  }
  return issues;
}

axk::ReportRow media_validation_issue(const CliLoaded &source, std::string severity,
                                      std::string code, std::string message, std::string scope,
                                      std::string sampler_path, std::string object_key,
                                      std::string quality, std::string basis,
                                      std::string recommended_next_check = {}) {
  return {
      {"severity", std::move(severity)},
      {"code", std::move(code)},
      {"message", std::move(message)},
      {"scope", std::move(scope)},
      {"source_path", axk::text::path_to_utf8(source.path)},
      {"sampler_path", std::move(sampler_path)},
      {"object_key", std::move(object_key)},
      {"quality", std::move(quality)},
      {"basis", std::move(basis)},
      {"recommended_next_check", std::move(recommended_next_check)},
  };
}

std::string media_object_report_path(const CliLoaded &source, std::string_view object_key) {
  if (const auto *item = catalog_object(source, object_key); item != nullptr && item->placement) {
    const auto &placement = *item->placement;
    const auto category = [&]() -> std::string_view {
      if (placement.category_name == "SMPL")
        return "Samples";
      if (placement.category_name == "SBNK")
        return "Sample Banks";
      if (placement.category_name == "SBAC")
        return "Sample Bank Accessories";
      if (placement.category_name == "SEQU")
        return "Sequences";
      if (placement.category_name == "PROG")
        return "Programs";
      return placement.category_name;
    }();
    std::string path = std::format("partition {}", placement.partition.value);
    for (const auto &component : {std::string_view{placement.volume_name}, category,
                                  std::string_view{placement.entry_name}}) {
      if (!component.empty())
        path += std::format("/{}", component);
    }
    return path;
  }
  const auto *object = media_object(source, object_key);
  return object == nullptr ? public_object_key(source, object_key) : object->logical_path;
}

std::string media_object_group_path(const CliLoaded &source, std::string_view object_key) {
  auto path = media_object_report_path(source, object_key);
  std::ranges::replace(path, '\\', '/');
  const auto filename_separator = path.rfind('/');
  if (filename_separator == std::string::npos)
    return path;
  const auto category_separator = path.rfind('/', filename_separator - 1U);
  if (category_separator == std::string::npos)
    return path;
  const auto category =
      path.substr(category_separator + 1U, filename_separator - category_separator - 1U);
  static constexpr std::array object_categories{"PROG", "SBAC", "SBNK", "SMPL", "SEQU", "PRF3"};
  if (std::ranges::find(object_categories, category) == object_categories.end())
    return path;
  return path.substr(0U, category_separator);
}

std::string active_program_assignment_label(const CliLoaded &source, const axk::Relationship &row) {
  const auto assignment_name = !row.assignment_name.empty()
                                   ? row.assignment_name
                                   : row.target_key.value_or("unnamed assignment");
  if (!row.assignment_index)
    return std::format("{}: {}", media_object_report_path(source, row.source_key), assignment_name);
  return std::format("{}: assignment {} {}", media_object_report_path(source, row.source_key),
                     *row.assignment_index + 1U, assignment_name);
}

std::string relationship_issue_path(const CliLoaded &source, const axk::Relationship &row) {
  if (row.type.starts_with("PROG_ASSIGNMENT_"))
    return active_program_assignment_label(source, row);
  return media_object_report_path(source, row.source_key);
}

std::pair<std::string, std::string> ambiguous_relationship_message(const axk::Relationship &row) {
  if (row.basis == "assignment-visible-off-same-volume-sbac-diagnostic" ||
      row.basis == "assignment-visible-off-same-volume-sbnk-diagnostic") {
    const auto target = row.basis == "assignment-visible-off-same-volume-sbac-diagnostic"
                            ? "sample-bank group"
                            : "sample-bank";
    return {std::format("Visible/off Program assignment row names a {} with one same-volume "
                        "diagnostic candidate plus other duplicate-name candidates; this is "
                        "decoded Program inventory, not active Program content loss.",
                        target),
            "Use relationships.csv candidate fields when auditing off rows; the same-volume "
            "candidate is diagnostic only and must not create an active Program child."};
  }
  if (row.assignment_state == axk::AssignmentState::visible_off)
    return {"Visible/off Program assignment row has multiple possible local targets; this is "
            "decoded Program inventory, not active Program content loss.",
            "Use relationships.csv candidate fields only when auditing off rows; do not treat "
            "this warning as a missing active Program child."};
  if (row.type == "PROG_ASSIGNMENT_TO_SBAC")
    return {"Program assignment to a sample-bank group has multiple possible targets.",
            "Verify the sampler-visible Program assignment and group target before promotion."};
  if (row.type == "PROG_ASSIGNMENT_TO_SBNK")
    return {"Direct Program assignment has multiple possible sample-bank targets.",
            "Verify the sampler-visible Program assignment target before promotion."};
  if (row.type == "SBAC_SLOT_TO_SBNK")
    return {"Sample-bank group slot has multiple possible sample-bank member targets.",
            "Inspect duplicate same-name sample-bank candidates before using this group slot as "
            "authoritative."};
  if (row.basis == "sbnk-member-link-id-only-iso-cross-folder-name-mismatch")
    return {"Sample-bank member link ID points to one physical waveform in another ISO object "
            "folder, but the member name does not confirm it.",
            "Inspect the waveform candidate before treating this member link as exact."};
  if (row.basis == "sbnk-member-link-id-only-name-mismatch")
    return {"Sample-bank member link ID points to one physical waveform, but the member name "
            "does not confirm it.",
            "Inspect the waveform candidate before treating this member link as exact."};
  if (row.type.starts_with("SBNK_") && row.type.ends_with("_TO_SMPL"))
    return {"Sample-bank member link has multiple possible physical waveform targets.",
            "Inspect candidate waveform objects before treating this sample-bank member link as "
            "exact."};
  if (row.basis.starts_with("sbnk-program-link-bitmap-")) {
    std::string message;
    if (row.basis.find("disambiguates-ambiguous-direct-assignment") != std::string::npos)
      message = "Sample-bank Program-link bitmap points to one Program from an ambiguous "
                "direct-assignment set.";
    else if (row.basis.find("known-direct-assignment-missing-bitmap") != std::string::npos)
      message =
          "Known direct Program assignment is missing from the sample-bank Program-link bitmap.";
    else if (row.basis.find("nondefault-flag-direct-assignment-without-bitmap") !=
             std::string::npos)
      message = "Nondefault direct Program assignment is missing from the sample-bank Program-link "
                "bitmap.";
    else
      message = "Sample-bank Program-link bitmap differs from resolved direct Program assignments.";
    return {std::move(message),
            "Use this as bitmap consistency data only; do not treat it as Program content loss "
            "unless another public rule proves the bitmap is authoritative."};
  }
  if (row.type == "SBNK_PROGRAM_BITMAP_TO_PROG")
    return {"Sample-bank Program-link bitmap maps to multiple possible Program slots.",
            "Use this as bitmap consistency data only until the Program target is disambiguated."};
  return {"Relationship has ambiguous candidate targets.",
          "Inspect candidate set before using for authoritative placement."};
}

std::string tentative_relationship_code(const axk::Relationship &row) {
  if (row.assignment_state == axk::AssignmentState::visible_off)
    return "REL_VISIBLE_OFF_ASSIGNMENT_DIAGNOSTIC";
  if (row.basis.starts_with("sbnk-program-link-bitmap-"))
    return "REL_PROGRAM_LINK_BITMAP_DIAGNOSTIC";
  if (row.basis.starts_with("sbnk-member-link-id-only"))
    return "REL_SBNK_MEMBER_LINK_DIAGNOSTIC";
  return "REL_AMBIGUOUS_TARGET";
}

std::pair<std::string, std::string> missing_relationship_message(const axk::Relationship &row) {
  if (row.assignment_state == axk::AssignmentState::active)
    return {"Active Program assignment references a missing local target.",
            "Inspect the Program assignment and source object group; user-facing info may show "
            "an unresolved placeholder instead of a normal Program child."};
  if (row.assignment_state == axk::AssignmentState::visible_off) {
    const auto expected =
        row.type == "PROG_ASSIGNMENT_TO_SBAC" ? "sample-bank group" : "sample-bank";
    return {std::format("Visible/off Program assignment row names a missing local {} target; "
                        "this is decoded Program inventory, not active Program content loss.",
                        expected),
            "Keep this row as diagnostic/off-row data unless sampler-visible checks prove it "
            "should become an active assignment."};
  }
  if (row.assignment_state == axk::AssignmentState::source_load)
    return {"Source-load Program assignment row has no resolved local target.",
            "Keep the selector as diagnostic source data until sampler-loaded placement or "
            "another public rule proves a target."};
  if (row.type.starts_with("SBNK_") && row.type.ends_with("_TO_SMPL"))
    return {"Sample-bank member link does not resolve to a physical waveform target.",
            "Inspect the object group before treating this sample-bank member as complete."};
  return {"Relationship target could not be resolved.",
          "Inspect the relationship row and decoded source object before treating the target as "
          "present."};
}

std::string missing_relationship_code(const axk::Relationship &row) {
  if (row.assignment_state == axk::AssignmentState::visible_off)
    return "REL_VISIBLE_OFF_ASSIGNMENT_DIAGNOSTIC";
  if (row.assignment_state == axk::AssignmentState::active)
    return "REL_ACTIVE_ASSIGNMENT_MISSING_TARGET";
  return "REL_MISSING_TARGET";
}

std::vector<axk::ReportRow> validate_media_details(const CliLoaded &source,
                                                   bool include_object_checks = true) {
  std::vector<axk::ReportRow> issues;
  if (include_object_checks) {
    for (const auto &issue : source.media.validation_issues()) {
      issues.push_back(media_validation_issue(source, "error", issue.code, issue.message,
                                              "container", issue.sampler_path, {}, "Confirmed",
                                              issue.basis, issue.recommended_next_check));
    }
    for (const auto &object : source.objects) {
      const auto required = static_cast<std::uint64_t>(object.decoded.header.header_size) +
                            object.decoded.header.payload_bytes_0x1c;
      if (required <= object.raw_payload.size())
        continue;
      issues.push_back(media_validation_issue(
          source, "error", "OBJECT_PAYLOAD_TRUNCATED",
          std::format("Object header requires {} bytes but payload has {} bytes.", required,
                      object.raw_payload.size()),
          "object", {}, public_object_key(source, object.key), "Known", "validation"));
    }
  }

  std::map<std::string, std::vector<std::string>> group_members;
  for (const auto &row : source.graph.relationships) {
    if (row.type == "SBAC_SLOT_TO_SBNK" && row.target_key &&
        (row.quality == axk::RelationshipQuality::known ||
         row.quality == axk::RelationshipQuality::likely)) {
      group_members[row.source_key].push_back(*row.target_key);
    }
  }
  std::map<std::string, std::vector<const axk::Relationship *>> reachable;
  for (const auto &row : source.graph.relationships) {
    if (!row.target_key ||
        (row.assignment_state != axk::AssignmentState::active &&
         row.assignment_state != axk::AssignmentState::source_load) ||
        (row.quality != axk::RelationshipQuality::known &&
         row.quality != axk::RelationshipQuality::likely)) {
      continue;
    }
    if (row.type == "PROG_ASSIGNMENT_TO_SBNK") {
      reachable[*row.target_key].push_back(&row);
    } else if (row.type == "PROG_ASSIGNMENT_TO_SBAC") {
      if (const auto members = group_members.find(*row.target_key);
          members != group_members.end()) {
        for (const auto &member : members->second)
          reachable[member].push_back(&row);
      }
    }
  }
  using MemberGroup = std::pair<std::string, bool>;
  std::map<MemberGroup, std::vector<const axk::Relationship *>> grouped_members;
  std::map<MemberGroup, std::set<std::string>> grouped_active_labels;
  std::set<std::string> covered_relationships;
  for (const auto &row : source.graph.relationships) {
    if ((row.type != "SBNK_LEFT_MEMBER_TO_SMPL" && row.type != "SBNK_RIGHT_MEMBER_TO_SMPL") ||
        row.quality != axk::RelationshipQuality::unknown)
      continue;
    const auto active = reachable.find(row.source_key);
    const MemberGroup group{media_object_group_path(source, row.source_key),
                            active != reachable.end()};
    grouped_members[group].push_back(&row);
    if (active != reachable.end()) {
      for (const auto *program_row : active->second)
        grouped_active_labels[group].insert(active_program_assignment_label(source, *program_row));
    }
    covered_relationships.insert(row.key);
  }
  for (const auto &[group, rows] : grouped_members) {
    std::set<std::string> source_keys;
    for (const auto *row : rows)
      source_keys.insert(public_object_key(source, row->source_key));
    const auto member_count = rows.size();
    const auto bank_count = source_keys.size();
    if (group.second) {
      std::string active_summary;
      const auto &labels = grouped_active_labels[group];
      std::size_t index{};
      for (const auto &label : labels) {
        if (index == 4U)
          break;
        if (!active_summary.empty())
          active_summary += "; ";
        active_summary += label;
        ++index;
      }
      if (labels.size() > 4U)
        active_summary += std::format("; +{} more", labels.size() - 4U);
      issues.push_back(media_validation_issue(
          source, "error", "REL_ACTIVE_PROGRAM_SBNK_MEMBER_TARGET_MISSING",
          std::format("{} sample-bank member link(s) across {} sample bank(s) do not resolve to "
                      "physical sample objects and are reachable from active Program assignments.",
                      member_count, bank_count),
          "relationship", std::format("{} | {}", group.first, active_summary), *source_keys.begin(),
          "Unknown", "SBNK member target aggregation",
          "Treat the affected Program/sample-bank path as incomplete until the missing physical "
          "sample objects are found or the source is confirmed partially loadable."));
    } else {
      issues.push_back(media_validation_issue(
          source, "warning", "REL_SBNK_MEMBER_TARGET_MISSING",
          std::format("{} sample-bank member link(s) across {} sample bank(s) do not resolve to "
                      "physical sample objects.",
                      member_count, bank_count),
          "relationship", group.first, *source_keys.begin(), "Unknown",
          "SBNK member target aggregation",
          "Inspect the sample-bank member links before treating this object group as complete."));
    }
  }
  for (const auto &row : source.graph.relationships) {
    if (covered_relationships.contains(row.key))
      continue;
    if (row.quality == axk::RelationshipQuality::tentative) {
      auto [message, next_check] = ambiguous_relationship_message(row);
      issues.push_back(media_validation_issue(
          source, "warning", tentative_relationship_code(row), std::move(message), "relationship",
          relationship_issue_path(source, row), public_object_key(source, row.source_key),
          "Tentative", row.basis, std::move(next_check)));
    } else if (row.quality == axk::RelationshipQuality::unknown) {
      auto [message, next_check] = missing_relationship_message(row);
      issues.push_back(media_validation_issue(
          source, "warning", missing_relationship_code(row), std::move(message), "relationship",
          relationship_issue_path(source, row), public_object_key(source, row.source_key),
          "Unknown", row.basis, std::move(next_check)));
    }
  }
  std::ranges::sort(issues, {}, [](const axk::ReportRow &row) {
    const auto value = [&](std::string_view key) -> std::string {
      const auto found = std::ranges::find(row, key, &axk::ReportRow::value_type::first);
      return found == row.end() ? std::string{} : std::get<std::string>(found->second.value);
    };
    return std::tuple{value("code"), value("object_key"), value("message")};
  });
  return issues;
}

int run_validate_request(const axk::cli::ValidateRequest &request) {
  if (!request.exports && request.paths.empty()) {
    std::cerr << "validate requires input paths unless --exports is supplied\n";
    return 2;
  }
  if (const auto ready = prepare_report_directory(request.output_directory, request.overwrite);
      !ready)
    return report_failure(ready.error());
  std::vector<axk::ReportRow> issues;
  std::vector<axk::ReportRow> allocation_summaries;
  std::vector<axk::ReportRow> allocation_extents;
  std::vector<axk::ReportRow> allocation_mismatches;
  std::vector<axk::ReportRow> volumes;
  std::vector<axk::ReportRow> volume_issues;
  std::map<std::string, std::uint64_t> issue_counts;
  bool failed{};
  bool has_sfs_input{};
  if (request.exports) {
    issues = validate_export_directory(*request.exports);
    for (const auto &issue : issues) {
      const auto code =
          std::ranges::find(issue, "code", &std::pair<std::string, axk::ReportValue>::first);
      if (code != issue.end())
        ++issue_counts[std::get<std::string>(code->second.value)];
    }
    failed = !issues.empty();
  }
  const auto loaded = request.exports ? CliLoadResult{} : load_cli_paths(request.paths);
  if (!loaded.errors.empty()) {
    std::cerr << "one or more validation inputs could not be opened\n";
    return 2;
  }
  for (const auto &source : loaded.loaded) {
    const auto &path = source.path;
    if (source.media.kind() != axk::MediaKind::sfs) {
      auto source_issues = validate_media_details(source);
      for (auto &issue : source_issues) {
        const auto code =
            std::ranges::find(issue, "code", &std::pair<std::string, axk::ReportValue>::first);
        if (code != issue.end())
          ++issue_counts[std::get<std::string>(code->second.value)];
        const auto severity =
            std::ranges::find(issue, "severity", &std::pair<std::string, axk::ReportValue>::first);
        if (severity != issue.end()) {
          const auto &value = std::get<std::string>(severity->second.value);
          if (value == "error" || value == "fatal" ||
              (request.policy == "strict" && value == "warning"))
            failed = true;
        }
        issues.push_back(std::move(issue));
      }
      continue;
    }
    has_sfs_input = true;
    const auto &container = std::get<axk::Container>(source.media.storage());
    const auto report = axk::validate_semantics(container, source.catalog, source.graph);
    for (const auto &issue : report.issues) {
      if (issue.code.starts_with("REL_"))
        continue;
      const auto severity = issue.severity == axk::ValidationSeverity::error     ? "error"
                            : issue.severity == axk::ValidationSeverity::warning ? "warning"
                                                                                 : "info";
      ++issue_counts[issue.code];
      if (issue.severity == axk::ValidationSeverity::error ||
          (request.policy == "strict" && issue.severity == axk::ValidationSeverity::warning))
        failed = true;
      issues.push_back({
          {"severity", severity},
          {"code", issue.code},
          {"message", issue.message},
          {"scope", "relationship"},
          {"source_path", axk::text::path_to_utf8(path)},
          {"sampler_path", issue.sampler_path},
          {"object_key", issue.object_key},
          {"quality", "Known"},
          {"basis", "validation"},
          {"recommended_next_check", ""},
      });
    }
    auto relationship_issues = validate_media_details(source, false);
    for (auto &issue : relationship_issues) {
      const auto code =
          std::ranges::find(issue, "code", &std::pair<std::string, axk::ReportValue>::first);
      if (code != issue.end())
        ++issue_counts[std::get<std::string>(code->second.value)];
      const auto severity =
          std::ranges::find(issue, "severity", &std::pair<std::string, axk::ReportValue>::first);
      if (severity != issue.end()) {
        const auto &value = std::get<std::string>(severity->second.value);
        if (value == "error" || value == "fatal" ||
            (request.policy == "strict" && value == "warning"))
          failed = true;
      }
      issues.push_back(std::move(issue));
    }
    auto source_summaries = allocation_summary_rows(path, container);
    std::ranges::move(source_summaries, std::back_inserter(allocation_summaries));
    auto source_extents = allocation_extent_rows(path, container);
    std::ranges::move(source_extents, std::back_inserter(allocation_extents));
    auto source_mismatches = allocation_mismatch_rows(path, container.partitions());
    std::ranges::move(source_mismatches, std::back_inserter(allocation_mismatches));
    const auto prior_issue_count = issues.size();
    auto source_volumes =
        volume_validation_rows(path, container, source.catalog, volume_issues, issues);
    for (auto index = prior_issue_count; index < issues.size(); ++index) {
      const auto code = std::ranges::find(issues[index], "code",
                                          &std::pair<std::string, axk::ReportValue>::first);
      if (code != issues[index].end())
        ++issue_counts[std::get<std::string>(code->second.value)];
      if (request.policy == "strict")
        failed = true;
    }
    std::ranges::move(source_volumes, std::back_inserter(volumes));
  }
  axk::ReportValue::Object summary_counts;
  for (const auto &[name, count] : issue_counts)
    summary_counts.emplace_back(name, count);
  const auto policy = request.policy;
  axk::ReportRow validation_summary{{"policy", policy},
                                    {"failed", failed},
                                    {"issue_count", static_cast<std::uint64_t>(issues.size())},
                                    {"summary_counts", std::move(summary_counts)}};
  std::uint64_t pass_count{};
  std::uint64_t warn_count{};
  std::uint64_t fail_count{};
  std::uint64_t fatal_issue_count{};
  std::uint64_t warning_issue_count{};
  std::uint64_t malformed_category_entry_count{};
  std::uint64_t allocation_issue_count{};
  for (const auto &row : volumes) {
    const auto text = [&](std::string_view key) -> std::string {
      const auto found =
          std::ranges::find(row, key, &std::pair<std::string, axk::ReportValue>::first);
      return found == row.end() ? std::string{} : std::get<std::string>(found->second.value);
    };
    const auto number = [&](std::string_view key) -> std::uint64_t {
      const auto found =
          std::ranges::find(row, key, &std::pair<std::string, axk::ReportValue>::first);
      return found == row.end() ? 0U : std::get<std::uint64_t>(found->second.value);
    };
    if (text("validation_status") == "Pass")
      ++pass_count;
    else if (text("validation_status") == "Warn")
      ++warn_count;
    else
      ++fail_count;
    fatal_issue_count += number("fatal_issue_count");
    warning_issue_count += number("warning_issue_count");
    malformed_category_entry_count += number("malformed_category_entry_count");
    allocation_issue_count += number("allocation_issue_count");
  }
  axk::ReportRow volume_summary{
      {"source_image",
       request.paths.size() == 1U ? axk::text::path_to_utf8(request.paths.front()) : ""},
      {"volume_count", static_cast<std::uint64_t>(volumes.size())},
      {"pass_count", pass_count},
      {"warn_count", warn_count},
      {"fail_count", fail_count},
      {"fatal_issue_count", fatal_issue_count},
      {"warning_issue_count", warning_issue_count},
      {"malformed_category_entry_count", malformed_category_entry_count},
      {"allocation_issue_count", allocation_issue_count},
  };
  std::vector<axk::ReportSchemaManifest> schemas;
  const auto report = [&](std::string name, const std::vector<axk::ReportRow> &rows) -> bool {
    auto schema = write_cli_report(request.output_directory, std::move(name), rows, "axklib",
                                   request.overwrite);
    if (!schema)
      return false;
    schemas.push_back(*schema);
    return true;
  };
  if (!report("validation_issues", issues))
    return 2;
  if (auto written = axk::write_report_object(request.output_directory / "validation_summary.json",
                                              validation_summary, request.overwrite);
      !written)
    return report_failure(written.error());
  axk::ReportSchemaOptions schema_options;
  schema_options.source_command = "axklib";
  schema_options.library_version = std::string{axk::version()};
  auto validation_summary_schema = axk::make_report_schema(
      "validation_summary", std::span{&validation_summary, 1U}, schema_options);
  if (auto written = axk::write_report_schema(request.output_directory / "_schemas" /
                                                  "validation_summary.schema.json",
                                              validation_summary_schema, request.overwrite);
      !written)
    return report_failure(written.error());
  schemas.push_back(validation_summary_schema);
  if (!request.exports && has_sfs_input) {
    if (!report("allocation_summary", allocation_summaries) ||
        !report("allocation_extents", allocation_extents) ||
        !report("allocation_mismatches", allocation_mismatches) ||
        !report("volume_validation", volumes) || !report("volume_validation_issues", volume_issues))
      return 2;
    const std::vector<axk::ReportRow> volume_summaries{volume_summary};
    if (!report("volume_validation_summary", volume_summaries))
      return 2;
  }
  if (auto index = axk::write_report_schema_index(
          request.output_directory / "_schemas" / "schema_index.json", schemas, request.overwrite);
      !index)
    return report_failure(index.error());
  std::cout << "issues=" << issues.size() << " failed=" << (failed ? "True" : "False")
            << " policy=" << policy << '\n';
  std::cout << "reports written to " << axk::text::path_to_utf8(request.output_directory) << '\n';
  return failed ? 1 : 0;
}

axk::Result<axk::ReportSchemaManifest> write_csv_schema(const std::filesystem::path &output,
                                                        std::string name,
                                                        std::span<const axk::ReportRow> rows,
                                                        bool overwrite) {
  if (auto written = axk::write_report_csv(output / (name + ".csv"), rows, {}, overwrite); !written)
    return std::unexpected{written.error()};
  axk::ReportSchemaOptions options;
  options.source_command = "axklib";
  options.library_version = std::string{axk::version()};
  auto schema = axk::make_report_schema(name, rows, options);
  if (auto written = axk::write_report_schema(output / "_schemas" / (name + ".schema.json"), schema,
                                              overwrite);
      !written)
    return std::unexpected{written.error()};
  return schema;
}

int run_corpus_audit_request(const axk::cli::CorpusAuditRequest &request) {
  if (const auto ready = prepare_report_directory(request.output_directory, request.overwrite);
      !ready)
    return report_failure(ready.error());
  const auto paths = expand_cli_paths(request.paths);
  const auto loaded = load_cli_paths(paths);
  std::vector<axk::ReportRow> manifest;
  for (const auto &path : paths) {
    std::error_code error;
    const auto exists = std::filesystem::exists(path, error);
    auto suffix = axk::text::path_to_utf8(path.extension());
    std::ranges::transform(suffix, suffix.begin(), [](const unsigned char value) {
      return static_cast<char>(std::tolower(value));
    });
    manifest.push_back({{"path", axk::text::path_to_utf8(path)},
                        {"exists", exists},
                        {"is_file", exists && std::filesystem::is_regular_file(path, error)},
                        {"is_dir", exists && std::filesystem::is_directory(path, error)},
                        {"suffix", suffix}});
  }
  std::vector<axk::ReportRow> inventory;
  std::vector<axk::ReportRow> relationships;
  std::vector<axk::ReportRow> validation_issues;
  std::vector<axk::ReportRow> wave_issues;
  std::uint64_t wave_decoded{};
  bool validation_failed{};
  for (const auto &source : loaded.loaded) {
    for (const auto &item : source.catalog.objects)
      inventory.push_back(inventory_row(source, item));
    for (const auto &row : source.graph.relationships)
      relationships.push_back(relationship_report_row(source, row));
    if (const auto *container = std::get_if<axk::Container>(&source.media.storage())) {
      const auto report = axk::validate_semantics(*container, source.catalog, source.graph);
      validation_failed = validation_failed || !report.valid();
      for (const auto &issue : report.issues) {
        const auto severity = issue.severity == axk::ValidationSeverity::error     ? "error"
                              : issue.severity == axk::ValidationSeverity::warning ? "warning"
                                                                                   : "info";
        validation_issues.push_back({
            {"severity", severity},
            {"code", issue.code},
            {"message", issue.message},
            {"scope", "relationship"},
            {"source_path", axk::text::path_to_utf8(source.path)},
            {"sampler_path", issue.sampler_path},
            {"object_key", issue.object_key},
            {"quality", "Known"},
            {"basis", "validation"},
            {"recommended_next_check", ""},
        });
      }
    }
    if (!request.skip_wave_smoke) {
      std::uint64_t successful{};
      for (const auto &item : source.catalog.objects) {
        if (item.object.header.type != axk::ObjectType::smpl)
          continue;
        axk::Result<axk::Waveform> waveform =
            source.media.kind() == axk::MediaKind::sfs
                ? axk::decode_waveform(std::get<axk::Container>(source.media.storage()), item)
                : [&]() -> axk::Result<axk::Waveform> {
          const auto object = std::ranges::find(source.objects, item.key, &axk::MediaObject::key);
          if (object == source.objects.end())
            return std::unexpected{axk::make_error(axk::ErrorCode::object_malformed,
                                                   axk::ErrorCategory::object,
                                                   "waveform object payload is unavailable")};
          return axk::decode_waveform(*object);
        }();
        if (waveform) {
          ++successful;
        } else {
          wave_issues.push_back({
              {"source_path", axk::text::path_to_utf8(source.path)},
              {"container_kind", media_kind_text(source.media.kind())},
              {"object_key", public_object_key(source, item.key)},
              {"sample_name", item.object.header.name},
              {"code", static_cast<std::uint64_t>(waveform.error().code)},
              {"severity", "error"},
              {"message", waveform.error().message},
          });
        }
      }
      wave_decoded += std::min(successful, static_cast<std::uint64_t>(request.wave_smoke_limit));
    }
  }
  const auto rows = relationship_rows(loaded);
  std::uint64_t ambiguous{};
  for (const auto &source : loaded.loaded) {
    ambiguous += static_cast<std::uint64_t>(
        std::ranges::count_if(source.graph.relationships, [](const auto &row) {
          return row.quality == axk::RelationshipQuality::tentative;
        }));
  }
  axk::ReportRow summary{
      {"input_count", static_cast<std::uint64_t>(loaded.loaded.size() + loaded.errors.size())},
      {"loaded_container_count", static_cast<std::uint64_t>(loaded.loaded.size())},
      {"load_error_count", static_cast<std::uint64_t>(loaded.errors.size())},
      {"relationship_load_error_count", static_cast<std::uint64_t>(loaded.errors.size())},
      {"object_count", static_cast<std::uint64_t>(inventory.size())},
      {"validation_issue_count", static_cast<std::uint64_t>(validation_issues.size())},
      {"validation_failed", validation_failed},
      {"relationship_count", static_cast<std::uint64_t>(rows.size())},
      {"ambiguous_relationship_count", ambiguous},
      {"wave_smoke_decoded", wave_decoded},
      {"wave_smoke_errors", static_cast<std::uint64_t>(wave_issues.size())},
  };
  if (auto written = axk::write_report_object(
          request.output_directory / "corpus_audit_summary.json", summary, request.overwrite);
      !written)
    return report_failure(written.error());
  if (auto written = axk::write_report_json(request.output_directory / "input_manifest.json",
                                            manifest, request.overwrite);
      !written)
    return report_failure(written.error());
  std::vector<axk::ReportSchemaManifest> schemas;
  axk::ReportSchemaOptions base_schema_options;
  base_schema_options.source_command = "axklib";
  base_schema_options.library_version = std::string{axk::version()};
  const std::array summary_rows{summary};
  auto summary_schema =
      axk::make_report_schema("corpus_audit_summary", summary_rows, base_schema_options);
  if (auto written = axk::write_report_schema(request.output_directory / "_schemas" /
                                                  "corpus_audit_summary.schema.json",
                                              summary_schema, request.overwrite);
      !written)
    return report_failure(written.error());
  schemas.push_back(summary_schema);
  const auto schema_for = [&](std::string name, std::span<const axk::ReportRow> report_rows,
                              bool json_already = false) -> bool {
    axk::Result<axk::ReportSchemaManifest> result =
        json_already ? [&]() -> axk::Result<axk::ReportSchemaManifest> {
      if (auto written = axk::write_report_csv(request.output_directory / (name + ".csv"),
                                               report_rows, {}, request.overwrite);
          !written)
        return std::unexpected{written.error()};
      axk::ReportSchemaOptions options;
      options.source_command = "axklib";
      options.library_version = std::string{axk::version()};
      auto schema = axk::make_report_schema(name, report_rows, options);
      if (auto written = axk::write_report_schema(request.output_directory / "_schemas" /
                                                      (name + ".schema.json"),
                                                  schema, request.overwrite);
          !written)
        return std::unexpected{written.error()};
      return schema;
    }()
        : write_csv_schema(request.output_directory, std::move(name), report_rows,
                           request.overwrite);
    if (!result)
      return false;
    schemas.push_back(*result);
    return true;
  };
  if (!schema_for("input_manifest", manifest, true) ||
      !schema_for("inventory_objects", inventory) ||
      !schema_for("validation_issues", validation_issues) ||
      !schema_for("relationships", relationships) || !schema_for("wave_smoke_issues", wave_issues))
    return 2;
  if (auto index = axk::write_report_schema_index(
          request.output_directory / "_schemas" / "schema_index.json", schemas, request.overwrite);
      !index)
    return report_failure(index.error());
  std::cout << "containers=" << loaded.loaded.size() << " objects=" << inventory.size()
            << " validation_issues=" << validation_issues.size()
            << " relationships=" << relationships.size() << " wave_smoke=" << wave_decoded << '/'
            << wave_issues.size() << '\n';
  std::cout << "reports written to " << axk::text::path_to_utf8(request.output_directory) << '\n';
  if (!loaded.errors.empty())
    return 3;
  return validation_failed ? 1 : 0;
}

} // namespace axk::cli::commands
