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

axk::Result<SemanticSnapshot> load_semantic_snapshot(const std::filesystem::path &path) {
    auto container = axk::open_image(path);
    if (!container)
        return std::unexpected{container.error()};
    auto catalog = axk::build_object_catalog(*container);
    if (!catalog)
        return std::unexpected{catalog.error()};
    auto graph = axk::build_relationship_graph(*catalog);
    return SemanticSnapshot{std::move(*container), std::move(*catalog), std::move(graph)};
}

std::string object_type_text(axk::ObjectType type) {
    switch (type) {
    case axk::ObjectType::smpl:
        return "SMPL";
    case axk::ObjectType::sbnk:
        return "SBNK";
    case axk::ObjectType::sbac:
        return "SBAC";
    case axk::ObjectType::prog:
        return "PROG";
    case axk::ObjectType::sequ:
        return "SEQU";
    case axk::ObjectType::prf3:
        return "PRF3";
    case axk::ObjectType::unknown:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

std::string media_kind_text(axk::MediaKind kind) {
    switch (kind) {
    case axk::MediaKind::sfs:
        return "sfs";
    case axk::MediaKind::fat12_floppy:
        return "fat12_floppy";
    case axk::MediaKind::iso9660:
        return "iso";
    case axk::MediaKind::standalone_object:
        return "standalone_object";
    }
    return "unknown";
}

std::vector<std::filesystem::path> expand_cli_paths(const std::vector<std::filesystem::path> &inputs) {
    static const std::set<std::string> extensions{".hda", ".hds", ".ima", ".img", ".iso"};
    std::vector<std::filesystem::path> result;
    for (const auto &path : inputs) {
        std::error_code error;
        if (std::filesystem::is_directory(path, error)) {
            for (std::filesystem::recursive_directory_iterator it{path, error}, end; it != end && !error;
                 it.increment(error)) {
                if (it->is_regular_file(error) && extensions.contains(axk::text::path_to_utf8(it->path().extension())))
                    result.push_back(it->path());
            }
        } else {
            result.push_back(path);
        }
    }
    std::ranges::sort(result, {}, [](const auto &path) { return axk::text::path_to_utf8(path); });
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

CliLoadResult load_cli_paths(const std::vector<std::filesystem::path> &inputs) {
    CliLoadResult result;
    for (const auto &path : expand_cli_paths(inputs)) {
        auto media = axk::open_media(path);
        if (!media) {
            result.errors.push_back({{"path", axk::text::path_to_utf8(path)},
                                     {"error_code", static_cast<std::uint64_t>(media.error().code)},
                                     {"message", media.error().message},
                                     {"recoverable", true},
                                     {"original_exception", "axk::Error"}});
            continue;
        }
        auto catalog = axk::build_object_catalog(*media);
        if (!catalog) {
            result.errors.push_back({{"path", axk::text::path_to_utf8(path)},
                                     {"error_code", static_cast<std::uint64_t>(catalog.error().code)},
                                     {"message", catalog.error().message},
                                     {"recoverable", true},
                                     {"original_exception", "axk::Error"}});
            continue;
        }
        auto objects = media->objects();
        if (!objects) {
            result.errors.push_back({{"path", axk::text::path_to_utf8(path)},
                                     {"error_code", static_cast<std::uint64_t>(objects.error().code)},
                                     {"message", objects.error().message},
                                     {"recoverable", true},
                                     {"original_exception", "axk::Error"}});
            continue;
        }
        auto graph = axk::build_relationship_graph(*catalog);
        result.loaded.push_back({path, std::move(*media), std::move(*objects), std::move(*catalog), std::move(graph)});
    }
    return result;
}

axk::Result<void> prepare_report_directory(const std::filesystem::path &path, bool overwrite) {
    std::error_code error;
    if (std::filesystem::exists(path, error) && !overwrite &&
        std::filesystem::directory_iterator{path, error} != std::filesystem::directory_iterator{}) {
        return std::unexpected{axk::make_error(axk::ErrorCode::io_open_failed, axk::ErrorCategory::io,
                                               "output directory is not empty: " + axk::text::path_to_utf8(path))};
    }
    std::filesystem::create_directories(path / "_schemas", error);
    if (error)
        return std::unexpected{axk::make_error(axk::ErrorCode::io_open_failed, axk::ErrorCategory::io,
                                               "could not create report output directory")};
    return {};
}

axk::Result<axk::ReportSchemaManifest> write_cli_report(const std::filesystem::path &output, std::string name,
                                                        std::span<const axk::ReportRow> rows,
                                                        std::string source_command, bool overwrite) {
    if (auto written = axk::write_report_csv(output / (name + ".csv"), rows, {}, overwrite); !written)
        return std::unexpected{written.error()};
    if (auto written = axk::write_report_json(output / (name + ".json"), rows, overwrite); !written)
        return std::unexpected{written.error()};
    axk::ReportSchemaOptions options;
    static_cast<void>(source_command);
    options.source_command = "axklib";
    options.library_version = std::string{axk::version()};
    if (name == "inventory_objects")
        options.semantic_notes = "Decoded object inventory rows produced "
                                 "through axklib.objects.decoded.";
    else if (name == "decode_issues")
        options.semantic_notes = "Decode issues use stable code/severity/quality fields.";
    else if (name == "objects")
        options.semantic_notes = "Filtered object summary rows produced "
                                 "through the canonical inventory view.";
    else if (name == "validation_issues")
        options.semantic_notes = "Validation issues use stable issue codes "
                                 "intended for regression and CI gates.";
    auto schema = axk::make_report_schema(name, rows, std::move(options));
    if (auto written = axk::write_report_schema(output / "_schemas" / (name + ".schema.json"), schema, overwrite);
        !written)
        return std::unexpected{written.error()};
    return schema;
}

std::string public_object_key(const CliLoaded &loaded, std::string_view native_key) {
    if (loaded.media.kind() == axk::MediaKind::sfs)
        return std::string{native_key};
    const auto object = std::ranges::find(loaded.objects, native_key, &axk::MediaObject::key);
    if (object == loaded.objects.end())
        return std::string{native_key};
    if (loaded.media.kind() == axk::MediaKind::fat12_floppy)
        return std::format("{}:{}", axk::text::path_to_utf8(loaded.path.filename()), object->logical_path);
    if (loaded.media.kind() == axk::MediaKind::iso9660)
        return std::format("{}:iso9660:{}", axk::text::path_to_utf8(loaded.path.filename()), object->logical_path);
    return std::format("{}:standalone-object", axk::text::path_to_utf8(loaded.path.filename()));
}

std::string public_scope_key(const CliLoaded &loaded, const axk::ObjectSnapshot &item) {
    if (loaded.media.kind() == axk::MediaKind::sfs)
        return std::format("{}:partition:{}", axk::text::path_to_utf8(loaded.path), item.partition.value);
    if (loaded.media.kind() == axk::MediaKind::fat12_floppy)
        return std::format("{}:fat-root", axk::text::path_to_utf8(loaded.path));
    if (loaded.media.kind() == axk::MediaKind::standalone_object)
        return std::format("{}:standalone-object", axk::text::path_to_utf8(loaded.path));
    const auto object = std::ranges::find(loaded.objects, item.key, &axk::MediaObject::key);
    return object == loaded.objects.end()
               ? std::format("{}:iso", axk::text::path_to_utf8(loaded.path))
               : std::format("{}:{}", axk::text::path_to_utf8(loaded.path), object->scope_key);
}

axk::ReportRow inventory_row(const CliLoaded &loaded, const axk::ObjectSnapshot &item) {
    const auto media_object = std::ranges::find(loaded.objects, item.key, &axk::MediaObject::key);
    const auto iso = loaded.media.kind() == axk::MediaKind::iso9660;
    const auto fat = loaded.media.kind() == axk::MediaKind::fat12_floppy;
    const auto sfs = loaded.media.kind() == axk::MediaKind::sfs;
    std::string decoded_kind{"UnknownObject"};
    std::string decoded_fields;
    if (item.object.header.type == axk::ObjectType::smpl) {
        decoded_kind = "DecodedSample";
        decoded_fields = "fine_tune;loop_length;loop_mode;loop_start;root_key;sample_rate";
    } else if (item.object.header.type == axk::ObjectType::sbnk) {
        decoded_kind = "DecodedSampleBank";
        decoded_fields = "bank_topology;left_sample_name;left_smpl_link_id";
    } else if (item.object.header.type == axk::ObjectType::sbac) {
        decoded_kind = "DecodedSampleBankAccessory";
        decoded_fields = "active_slot_count;max_slot_count_from_payload";
    } else if (item.object.header.type == axk::ObjectType::prog) {
        decoded_kind = "DecodedProgram";
        decoded_fields = "control_record_count";
    } else if (item.object.header.type == axk::ObjectType::sequ) {
        decoded_kind = "DecodedSequence";
    }
    const auto field_count =
        decoded_fields.empty() ? 0U : static_cast<unsigned int>(std::ranges::count(decoded_fields, ';') + 1);
    std::uint64_t payload_offset{};
    if (sfs) {
        const auto &container = std::get<axk::Container>(loaded.media.storage());
        const auto partition = std::ranges::find(container.partitions(), item.partition.value,
                                                 [](const auto &row) { return row.index.value; });
        if (partition != container.partitions().end()) {
            const auto record = std::ranges::find(partition->records, item.sfs_id.value,
                                                  [](const auto &row) { return row.sfs_id.value; });
            if (record != partition->records.end() && !record->extents.empty()) {
                payload_offset = (static_cast<std::uint64_t>(partition->start_sector) +
                                  static_cast<std::uint64_t>(record->extents.front().cluster_offset) *
                                      partition->sectors_per_cluster) *
                                 container.superblock().sector_size_bytes;
            }
        }
    }
    const auto payload_size =
        media_object != loaded.objects.end()
            ? static_cast<std::uint64_t>(media_object->raw_payload.size())
            : static_cast<std::uint64_t>(item.object.header.header_size) + item.object.header.payload_bytes_0x1c;
    const axk::FatFile *fat_metadata{};
    if (fat && media_object != loaded.objects.end()) {
        const auto *image = std::get_if<axk::FatImage>(&loaded.media.storage());
        if (image != nullptr) {
            const auto found = std::ranges::find(image->files(), media_object->logical_path, &axk::FatFile::path);
            if (found != image->files().end())
                fat_metadata = &*found;
        }
    }
    return {
        {"source_path", axk::text::path_to_utf8(loaded.path)},
        {"container_kind", media_kind_text(loaded.media.kind())},
        {"detected_format", media_kind_text(loaded.media.kind())},
        {"scope_key", public_scope_key(loaded, item)},
        {"object_key", public_object_key(loaded, item.key)},
        {"partition_index",
         sfs ? axk::ReportValue{static_cast<std::uint64_t>(item.partition.value)} : axk::ReportValue{""}},
        {"sfs_id", sfs ? axk::ReportValue{static_cast<std::uint64_t>(item.sfs_id.value)} : axk::ReportValue{""}},
        {"fat_file", !sfs && media_object != loaded.objects.end() ? axk::ReportValue{media_object->logical_path}
                                                                  : axk::ReportValue{""}},
        {"payload_offset", sfs                                    ? axk::ReportValue{payload_offset}
                           : media_object != loaded.objects.end() ? axk::ReportValue{media_object->data_offset}
                                                                  : axk::ReportValue{""}},
        {"payload_size", payload_size},
        {"object_type", object_type_text(item.object.header.type)},
        {"object_name", item.object.header.name},
        {"object_format", "normal-fsfsdev3splx"},
        {"decoded_kind", decoded_kind},
        {"decoded_field_count", static_cast<std::uint64_t>(field_count)},
        {"decoded_fields", decoded_fields},
        {"decode_issue_count", std::uint64_t{0}},
        {"decode_issue_codes", ""},
        {"iso_extent_sector", iso && media_object != loaded.objects.end()
                                  ? axk::ReportValue{media_object->data_offset / 2048U}
                                  : axk::ReportValue{""}},
        {"iso_data_offset", iso && media_object != loaded.objects.end() ? axk::ReportValue{media_object->data_offset}
                                                                        : axk::ReportValue{""}},
        {"iso_file_size",
         iso && media_object != loaded.objects.end() ? axk::ReportValue{media_object->size} : axk::ReportValue{""}},
        {"iso_recovery_quality", iso ? axk::ReportValue{"clean-iso9660-object"} : axk::ReportValue{""}},
        {"iso_raw_group", iso && media_object != loaded.objects.end() ? axk::ReportValue{media_object->raw_group}
                                                                      : axk::ReportValue{""}},
        {"iso_raw_volume", iso && media_object != loaded.objects.end() ? axk::ReportValue{media_object->raw_volume}
                                                                       : axk::ReportValue{""}},
        {"iso_group_label", iso && media_object != loaded.objects.end()
                                ? axk::ReportValue{media_object->group_label.value}
                                : axk::ReportValue{""}},
        {"iso_volume_label", iso && media_object != loaded.objects.end()
                                 ? axk::ReportValue{media_object->volume_label.value}
                                 : axk::ReportValue{""}},
        {"iso_group_label_source",
         iso && media_object != loaded.objects.end() && media_object->group_label.status == axk::LabelStatus::confirmed
             ? axk::ReportValue{"yamaha-cdrom-menu-label"}
             : axk::ReportValue{""}},
        {"iso_volume_label_source",
         iso && media_object != loaded.objects.end() && media_object->volume_label.status == axk::LabelStatus::confirmed
             ? axk::ReportValue{"yamaha-cdrom-menu-label"}
             : axk::ReportValue{""}},
        {"fat_directory_offset",
         fat_metadata != nullptr ? axk::ReportValue{fat_metadata->directory_offset} : axk::ReportValue{""}},
        {"fat_first_cluster", fat_metadata != nullptr
                                  ? axk::ReportValue{static_cast<std::uint64_t>(fat_metadata->first_cluster)}
                                  : axk::ReportValue{""}},
        {"fat_cluster_count", fat_metadata != nullptr
                                  ? axk::ReportValue{static_cast<std::uint64_t>(fat_metadata->clusters.size())}
                                  : axk::ReportValue{""}},
        {"fat_file_size",
         fat && media_object != loaded.objects.end() ? axk::ReportValue{media_object->size} : axk::ReportValue{""}},
        {"fat_object_offset", fat && media_object != loaded.objects.end() ? axk::ReportValue{media_object->data_offset}
                                                                          : axk::ReportValue{""}},
        {"fat_stored_payload_offset", fat && media_object != loaded.objects.end()
                                          ? axk::ReportValue{media_object->data_offset + item.object.header.header_size}
                                          : axk::ReportValue{""}}};
}

axk::ReportRow relationship_report_row(const CliLoaded &loaded, const axk::Relationship &row) {
    std::string target;
    if (row.target_key)
        target = public_object_key(loaded, *row.target_key);
    else {
        for (const auto &candidate : row.candidate_keys) {
            if (!target.empty())
                target += '|';
            target += public_object_key(loaded, candidate);
        }
    }
    const auto source_key = public_object_key(loaded, row.source_key);
    std::string raw_fields;
    std::string notes = row.notes;
    const auto source = std::ranges::find(loaded.catalog.objects, row.source_key, &axk::ObjectSnapshot::key);
    if (source != loaded.catalog.objects.end() &&
        (row.type == "SBNK_LEFT_MEMBER_TO_SMPL" || row.type == "SBNK_RIGHT_MEMBER_TO_SMPL")) {
        if (const auto *bank = std::get_if<axk::CurrentSbnk>(&source->object.payload)) {
            const bool right = row.type == "SBNK_RIGHT_MEMBER_TO_SMPL";
            const auto *member = right && bank->right ? &*bank->right : &bank->left;
            raw_fields = std::format("SBNK+{} member {}; name={}; link_id=0x{:08x}", right ? "right" : "left",
                                     right ? "right" : "left", member->sample_name, member->smpl_link_id);
            if (row.basis == "sbnk-member-link+name")
                notes = "Current SBNK member name and member link ID match "
                        "exactly one same-scope SMPL "
                        "object.";
        }
    } else if (source != loaded.catalog.objects.end() && row.type == "SBAC_SLOT_TO_SBNK") {
        if (const auto *group = std::get_if<axk::CurrentSbac>(&source->object.payload)) {
            std::size_t index{};
            if (row.target_key) {
                const auto target_object =
                    std::ranges::find(loaded.catalog.objects, *row.target_key, &axk::ObjectSnapshot::key);
                if (target_object != loaded.catalog.objects.end()) {
                    const auto found =
                        std::ranges::find(group->slots, target_object->object.header.name, &axk::SbacSlot::name);
                    if (found != group->slots.end())
                        index = static_cast<std::size_t>(std::distance(group->slots.begin(), found));
                }
            }
            const auto offset = index < group->slots.size() ? group->slots[index].offset : 0x14cU;
            raw_fields = std::format("SBAC slot {} at 0x{:03x}", index, offset);
            if (row.basis == "active-sbac-slot-name")
                notes = "Input consistency: counted SBAC slot name uniquely "
                        "matches a same-scope SBNK "
                        "header name. The companion 32-bit slot word is "
                        "preserved as raw/opaque.";
        }
    } else if (row.type == "SBNK_PROGRAM_BITMAP_TO_PROG") {
        raw_fields = "SBNK+0x0c0..0x0cf";
        notes = "Validated standalone assignment rows support "
                "SBNK+0x0c0..0x0cf as four "
                "big-endian program-link bitmap words for direct "
                "PROG->SBNK/sample assignments. "
                "PROG->SBAC assignments are reported separately as indirection "
                "and are not expected "
                "to set child SBNK bits.";
    } else if (row.assignment_index) {
        raw_fields = std::format("PROG assignment {} at 0x{:03x}", *row.assignment_index,
                                 0x120U + static_cast<unsigned int>(*row.assignment_index) * 0x38U);
    }
    std::string diagnostic;
    if (row.assignment_state == axk::AssignmentState::visible_off)
        diagnostic = "visible-off-assignment";
    else if (row.basis == "assignment-active-missing-local-target")
        diagnostic = "active-assignment-missing-target";
    else if (row.basis.starts_with("sbnk-program-link-bitmap-"))
        diagnostic = "program-link-bitmap";
    else if (row.quality == axk::RelationshipQuality::tentative)
        diagnostic = row.basis.starts_with("sbnk-member-link-id-only") ? "sbnk-member-link" : "ambiguous-target";
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
            {"source_image", axk::text::path_to_utf8(loaded.path)},
            {"scope_key", std::format("{}:{}", axk::text::path_to_utf8(loaded.path), row.scope_key)},
            {"assignment_index", row.assignment_index
                                     ? axk::ReportValue{static_cast<std::uint64_t>(*row.assignment_index)}
                                     : axk::ReportValue{nullptr}},
            {"assignment_name", row.assignment_name},
            {"assignment_row_state", row.assignment_index ? "decoded-row" : ""},
            {"active_assignment_state",
             row.assignment_index ? std::string{axk::assignment_state_name(row.assignment_state)} : std::string{}},
            {"assignment_rch_assign_display", row.receive_channel_display},
            {"diagnostic_category", diagnostic}};
}

int report_failure(const axk::Error &error) {
    std::cerr << axk::render_error(error) << '\n';
    if (error.message.starts_with("output directory is not empty") ||
        error.message.starts_with("refusing to replace existing report"))
        return 1;
    return 2;
}

} // namespace axk::cli::commands
