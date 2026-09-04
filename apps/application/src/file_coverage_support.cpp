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

std::string joined_strings(const std::vector<std::string> &items) {
    std::string result;
    for (const auto &item : items) {
        if (!result.empty())
            result += '|';
        result += item;
    }
    return result;
}

axk::ReportRow relationship_report_row(const LoadedSource &source, const axk::Relationship &row,
                                       std::string_view display_path) {
    std::string target;
    if (row.target_key) {
        target = public_object_key(source, *row.target_key);
    } else {
        for (const auto &candidate : row.candidate_keys) {
            if (!target.empty())
                target += '|';
            target += public_object_key(source, candidate);
        }
    }
    const auto source_key = public_object_key(source, row.source_key);
    std::string raw_fields;
    std::string notes = row.notes;
    const auto source_object =
        std::ranges::find(source.inventory.catalog.objects, row.source_key, &axk::ObjectSnapshot::key);
    if (source_object != source.inventory.catalog.objects.end() &&
        (row.type == "SBNK_LEFT_MEMBER_TO_SMPL" || row.type == "SBNK_RIGHT_MEMBER_TO_SMPL")) {
        if (const auto *sample = std::get_if<axk::CurrentSbnk>(&source_object->object.payload)) {
            const bool right = row.type == "SBNK_RIGHT_MEMBER_TO_SMPL";
            const auto *member = right && sample->right ? &*sample->right : &sample->left;
            raw_fields =
                std::format("SBNK+{} member {}; name={}; cached_reference_value=0x{:08x}", right ? "right" : "left",
                            right ? "right" : "left", member->wave_data_name, member->cached_wave_data_reference_value);
        }
    } else if (source_object != source.inventory.catalog.objects.end() && row.type == "SBAC_SLOT_TO_SBNK") {
        if (const auto *sample_bank = std::get_if<axk::CurrentSbac>(&source_object->object.payload)) {
            std::size_t index{};
            if (row.target_key) {
                const auto target_object =
                    std::ranges::find(source.inventory.catalog.objects, *row.target_key, &axk::ObjectSnapshot::key);
                if (target_object != source.inventory.catalog.objects.end()) {
                    const auto found =
                        std::ranges::find(sample_bank->slots, target_object->object.header.name, &axk::SbacSlot::name);
                    if (found != sample_bank->slots.end())
                        index = static_cast<std::size_t>(std::distance(sample_bank->slots.begin(), found));
                }
            }
            const auto offset = index < sample_bank->slots.size() ? sample_bank->slots[index].offset : 0x14cU;
            raw_fields = std::format("SBAC slot {} at 0x{:03x}", index, offset);
            if (row.basis == "active-sbac-slot-name") {
                notes = "Input consistency: counted SBAC slot name uniquely matches a same-scope SBNK header name. "
                        "The companion 32-bit slot word is preserved as transient runtime-pointer residue and is "
                        "not used as resolver input.";
            }
        }
    } else if (row.type == "SBNK_PROGRAM_BITMAP_TO_PROG") {
        raw_fields = "SBNK+0x0c0..0x0cf";
        notes = "Validated standalone assignment rows support SBNK+0x0c0..0x0cf as four big-endian program-link "
                "bitmap words for direct PROG->SBNK/sample assignments. PROG->SBAC assignments are reported "
                "separately as indirection and are not expected to set child SBNK bits.";
    } else if (row.assignment_index) {
        raw_fields = std::format("PROG assignment {} at 0x{:03x}", *row.assignment_index,
                                 0x120U + static_cast<unsigned int>(*row.assignment_index) * 0x38U);
    }
    std::string diagnostic;
    if (row.basis == "assignment-stored-missing-local-target")
        diagnostic = "stored-program-row-missing-target";
    else if (row.basis.starts_with("sbnk-program-link-bitmap-"))
        diagnostic = "program-link-bitmap";
    else if (row.quality == axk::RelationshipQuality::tentative)
        diagnostic = row.basis == "sbnk-member-cache-only-name-mismatch" ? "sbnk-member-cache" : "ambiguous-target";
    else if (row.quality == axk::RelationshipQuality::unknown)
        diagnostic = "missing-target";
    return {{"key", std::format("{}|{}|{}|{}", source_key, row.type, target.empty() ? "missing" : target, row.basis)},
            {"source_key", source_key},
            {"target_key", target},
            {"relationship_type", row.type},
            {"quality", std::string{axk::relationship_quality_name(row.quality)}},
            {"basis", row.basis},
            {"raw_fields", raw_fields},
            {"ambiguity_notes", notes},
            {"source_image", std::string{display_path}},
            {"scope_key", std::format("{}:{}", display_path, row.scope_key)},
            {"assignment_index", row.assignment_index
                                     ? axk::ReportValue{static_cast<std::uint64_t>(*row.assignment_index)}
                                     : axk::ReportValue{nullptr}},
            {"assignment_name", row.assignment_name},
            {"assignment_row_state", row.assignment_index ? "decoded-row" : ""},
            {"assignment_state",
             row.assignment_index ? std::string{axk::assignment_state_name(row.assignment_state)} : std::string{}},
            {"assignment_rch_assign_display", row.receive_channel_display},
            {"diagnostic_category", diagnostic}};
}

std::size_t program_ignored_count(const LoadedSource &source) {
    std::size_t result{};
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
        for (std::size_t index = 0; index < program->assignments.size(); ++index) {
            const auto &assignment = program->assignments[index];
            if (assignment.name.empty() || represented.contains(index))
                continue;
            const bool known_kind = assignment.kind == 0x10U || assignment.kind == 0x11U;
            const bool name_match = std::ranges::any_of(source.inventory.catalog.objects, [&](const auto &target) {
                return target.scope_key == item.scope_key && target.object.header.name == assignment.name;
            });
            if ((!known_kind && !name_match) || assignment.raw_handle == 0U)
                ++result;
        }
    }
    return result;
}

const axk::ObjectSnapshot *catalog_object(const LoadedSource &source, std::string_view key) {
    const auto found = std::ranges::find(source.inventory.catalog.objects, key, &axk::ObjectSnapshot::key);
    return found == source.inventory.catalog.objects.end() ? nullptr : &*found;
}

const axk::MediaObjectDescriptor *media_object(const LoadedSource &source, std::string_view key) {
    const auto found = std::ranges::find(source.inventory.objects, key, &axk::MediaObjectDescriptor::key);
    return found == source.inventory.objects.end() ? nullptr : &*found;
}

const axk::FatFile *fat_file_metadata(const LoadedSource &source, const axk::MediaObjectDescriptor *object) {
    if (object == nullptr)
        return nullptr;
    const auto *fat = std::get_if<axk::FatImage>(&source.media.storage());
    if (fat == nullptr)
        return nullptr;
    const auto found = std::ranges::find(fat->files(), object->logical_path, &axk::FatFile::path);
    return found == fat->files().end() ? nullptr : &*found;
}

std::uint64_t sfs_payload_offset(const LoadedSource &source, const axk::ObjectSnapshot &item) {
    if (source.media.kind() != axk::MediaKind::sfs)
        return 0U;
    const auto &container = std::get<axk::Container>(source.media.storage());
    const auto partition = std::ranges::find(container.partitions(), item.partition.value,
                                             [](const auto &row) { return row.index.value; });
    if (partition == container.partitions().end())
        return 0U;
    const auto record =
        std::ranges::find(partition->records, item.sfs_id.value, [](const auto &row) { return row.sfs_id.value; });
    if (record == partition->records.end() || record->extents.empty())
        return 0U;
    return (static_cast<std::uint64_t>(partition->start_sector) +
            static_cast<std::uint64_t>(record->extents.front().cluster_offset) * partition->sectors_per_cluster) *
           container.superblock().sector_size_bytes;
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

std::vector<axk::ReportRow> sbac_detail_rows(std::span<const LoadedSource> sources,
                                             const axk::app::OperationContext &context) {
    std::vector<axk::ReportRow> rows;
    for (const auto &source : sources) {
        const auto display_path = source_display_path(source.source, context);
        for (const auto &relation : source.graph.relationships) {
            if (relation.type != "SBAC_SLOT_TO_SBNK")
                continue;
            const auto *sbac_item = catalog_object(source, relation.source_key);
            if (sbac_item == nullptr)
                continue;
            const auto *sbac = std::get_if<axk::CurrentSbac>(&sbac_item->object.payload);
            if (sbac == nullptr)
                continue;
            const auto matched = relation.target_key ? catalog_object(source, *relation.target_key) : nullptr;
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
                                         ? "Input consistency: counted SBAC slot name uniquely matches a same-scope "
                                           "SBNK header name. The companion 32-bit slot word is preserved as "
                                           "transient runtime-pointer residue and is not used as resolver input."
                                         : relation.notes;
            rows.push_back({
                {"image", display_path},
                {"container_kind", info_media_kind_name(source.media.kind())},
                {"scope_key", public_scope_key(source, *sbac_item, display_path)},
                {"sbac_object_key", public_object_key(source, sbac_item->key)},
                {"sbac_partition_index", optional_unsigned(sfs, sbac_item->partition.value)},
                {"sbac_sfs_id", optional_unsigned(sfs, sbac_item->sfs_id.value)},
                {"sbac_fat_file", fat && sbac_media != nullptr ? sbac_media->logical_path : ""},
                {"sbac_payload_offset",
                 optional_unsigned(sfs || sbac_media != nullptr,
                                   sfs ? sfs_payload_offset(source, *sbac_item) : sbac_media->data_offset)},
                {"sbac_name", sbac_item->object.header.name},
                {"sbac_payload_size", sbac_media != nullptr ? sbac_media->size : std::uint64_t{0}},
                {"sbac_stored_member_count_0x144", static_cast<std::uint64_t>(sbac->stored_member_count)},
                {"slot_index", static_cast<std::uint64_t>(slot_index)},
                {"slot_offset", static_cast<std::uint64_t>(slot->offset)},
                {"slot_sbnk_name", slot->name},
                {"slot_transient_member_pointer_0x10", static_cast<std::uint64_t>(slot->transient_member_pointer)},
                {"match_method", relation.basis},
                {"match_quality", std::string{axk::relationship_quality_name(relation.quality)}},
                {"match_notes", match_notes},
                {"candidate_count", static_cast<std::uint64_t>(relation.candidate_keys.size())},
                {"candidate_object_keys", joined_strings(candidate_keys)},
                {"candidate_fat_files", fat ? joined_strings(candidate_files) : ""},
                {"candidate_names", joined_strings(candidate_names)},
                {"matched_sbnk_object_key", matched == nullptr ? "" : public_object_key(source, matched->key)},
                {"matched_sbnk_partition_index",
                 optional_unsigned(sfs && matched != nullptr, matched == nullptr ? 0U : matched->partition.value)},
                {"matched_sbnk_sfs_id",
                 optional_unsigned(sfs && matched != nullptr, matched == nullptr ? 0U : matched->sfs_id.value)},
                {"matched_sbnk_fat_file", fat && matched_media != nullptr ? matched_media->logical_path : ""},
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
                 optional_unsigned(iso && sbac_media != nullptr, sbac_media == nullptr ? 0U : sbac_media->data_offset)},
                {"sbac_iso_file_size",
                 optional_unsigned(iso && sbac_media != nullptr, sbac_media == nullptr ? 0U : sbac_media->size)},
                {"sbac_iso_recovery_quality", iso ? "clean-iso9660-object" : ""},
                {"sbac_fat_directory_offset",
                 optional_unsigned(sbac_fat != nullptr, sbac_fat == nullptr ? 0U : sbac_fat->directory_offset)},
                {"sbac_fat_first_cluster",
                 optional_unsigned(sbac_fat != nullptr, sbac_fat == nullptr ? 0U : sbac_fat->first_cluster)},
                {"sbac_fat_cluster_count",
                 optional_unsigned(sbac_fat != nullptr, sbac_fat == nullptr ? 0U : sbac_fat->clusters.size())},
                {"sbac_fat_file_size",
                 optional_unsigned(fat && sbac_media != nullptr, sbac_media == nullptr ? 0U : sbac_media->size)},
                {"sbac_fat_object_offset",
                 optional_unsigned(fat && sbac_media != nullptr, sbac_media == nullptr ? 0U : sbac_media->data_offset)},
                {"sbac_fat_stored_payload_offset",
                 optional_unsigned(sbac_fat != nullptr, sbac_fat == nullptr ? 0U : sbac_fat->first_data_offset)},
                {"matched_sbnk_iso_extent_sector",
                 optional_unsigned(iso && matched_media != nullptr,
                                   matched_media == nullptr ? 0U : matched_media->data_offset / 2048U)},
                {"matched_sbnk_iso_data_offset",
                 optional_unsigned(iso && matched_media != nullptr,
                                   matched_media == nullptr ? 0U : matched_media->data_offset)},
                {"matched_sbnk_iso_file_size", optional_unsigned(iso && matched_media != nullptr,
                                                                 matched_media == nullptr ? 0U : matched_media->size)},
                {"matched_sbnk_iso_recovery_quality", iso ? "clean-iso9660-object" : ""},
                {"matched_sbnk_fat_directory_offset",
                 optional_unsigned(matched_fat != nullptr,
                                   matched_fat == nullptr ? 0U : matched_fat->directory_offset)},
                {"matched_sbnk_fat_first_cluster",
                 optional_unsigned(matched_fat != nullptr, matched_fat == nullptr ? 0U : matched_fat->first_cluster)},
                {"matched_sbnk_fat_cluster_count",
                 optional_unsigned(matched_fat != nullptr, matched_fat == nullptr ? 0U : matched_fat->clusters.size())},
                {"matched_sbnk_fat_file_size", optional_unsigned(fat && matched_media != nullptr,
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

std::vector<axk::ReportRow> bitmap_detail_rows(std::span<const LoadedSource> sources,
                                               const axk::app::OperationContext &context) {
    static constexpr std::string_view notes =
        "Validated standalone assignment rows support SBNK+0x0c0..0x0cf as four big-endian program-link bitmap "
        "words for direct PROG->SBNK/sample assignments. PROG->SBAC assignments are reported separately as "
        "indirection and are not expected to set child SBNK bits.";
    std::vector<axk::ReportRow> rows;
    for (const auto &source : sources) {
        const auto display_path = source_display_path(source.source, context);
        for (const auto &comparison : source.graph.bitmap_comparisons) {
            const auto *item = catalog_object(source, comparison.sbnk_key);
            if (item == nullptr)
                continue;
            const auto *sample = std::get_if<axk::CurrentSbnk>(&item->object.payload);
            if (sample == nullptr)
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
                const auto detail = std::format("{}@slot{}:kind0x{:02x}:flag0x{:02x}", program_item->object.header.name,
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
            direct_details.erase(std::unique(direct_details.begin(), direct_details.end()), direct_details.end());
            ambiguous_programs.erase(std::unique(ambiguous_programs.begin(), ambiguous_programs.end()),
                                     ambiguous_programs.end());
            ambiguous_details.erase(std::unique(ambiguous_details.begin(), ambiguous_details.end()),
                                    ambiguous_details.end());
            rows.push_back({
                {"image", display_path},
                {"container_kind", info_media_kind_name(source.media.kind())},
                {"scope_key", public_scope_key(source, *item, display_path)},
                {"sbnk_object_key", public_object_key(source, item->key)},
                {"sbnk_partition_index", optional_unsigned(sfs, item->partition.value)},
                {"sbnk_sfs_id", optional_unsigned(sfs, item->sfs_id.value)},
                {"sbnk_fat_file", fat && object != nullptr ? object->logical_path : ""},
                {"sbnk_payload_offset",
                 optional_unsigned(sfs || object != nullptr,
                                   sfs ? sfs_payload_offset(source, *item) : object->data_offset)},
                {"sbnk_name", item->object.header.name},
                {"linked_programs_001_032_bitmap_0x0c0",
                 static_cast<std::uint64_t>(sample->linked_program_bitmap_words[0])},
                {"linked_programs_033_064_bitmap_0x0c4",
                 static_cast<std::uint64_t>(sample->linked_program_bitmap_words[1])},
                {"linked_programs_065_096_bitmap_0x0c8",
                 static_cast<std::uint64_t>(sample->linked_program_bitmap_words[2])},
                {"linked_programs_097_128_bitmap_0x0cc",
                 static_cast<std::uint64_t>(sample->linked_program_bitmap_words[3])},
                {"bitmap_programs", joined_programs(comparison.bitmap_programs)},
                {"direct_prog_assignment_programs", joined_programs(comparison.direct_assignment_programs)},
                {"direct_prog_assignment_details", joined_strings(direct_details)},
                {"ambiguous_direct_assignment_programs", joined_strings(ambiguous_programs)},
                {"ambiguous_direct_assignment_details", joined_strings(ambiguous_details)},
                {"sbac_indirect_assignment_programs", joined_programs(comparison.indirect_assignment_programs)},
                {"bitmap_without_direct_assignment_programs", joined_programs(comparison.bitmap_without_direct)},
                {"direct_assignment_without_bitmap_programs", joined_programs(comparison.direct_without_bitmap)},
                {"mismatch_class", comparison.mismatch_class},
                {"match_status", comparison.status},
                {"quality", comparison.status == "match" ? "Known" : "Tentative"},
                {"notes", std::string{notes}},
            });
        }
    }
    return rows;
}

} // namespace axk::app::file_operations_internal
