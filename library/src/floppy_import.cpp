#include "axklib/floppy_import.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <utility>

#include "axklib/floppy_catalog_internal.hpp"
#include "axklib/package_closure.hpp"
#include "axklib/package_graph.hpp"
#include "axklib/package_relocation.hpp"
#include "floppy_import_recovery.hpp"
#include "media_internal.hpp"

namespace axk {
namespace {
Error invalid(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::manifest, std::move(message));
}

std::optional<PackageRootKind> root_kind(ObjectType type) {
    switch (type) {
    case ObjectType::prog:
        return PackageRootKind::prog;
    case ObjectType::sbac:
        return PackageRootKind::sbac;
    case ObjectType::sbnk:
        return PackageRootKind::sbnk;
    case ObjectType::smpl:
        return PackageRootKind::smpl;
    case ObjectType::sequ:
        return PackageRootKind::sequ;
    default:
        return std::nullopt;
    }
}

Result<void> inspect_objects(FloppyImportInspection &inspection, const ObjectCatalog &catalog,
                             const RelationshipGraph &graph, const detail::FloppyFileSources &source_files,
                             const CancellationToken &cancellation) {
    std::map<std::string, const ObjectSnapshot *, std::less<>> objects;
    for (const auto &object : catalog.objects)
        objects.emplace(object.key, &object);
    std::map<std::string, std::size_t, std::less<>> indices;
    for (const auto &object : catalog.objects) {
        if (const auto checked = cancellation.check(); !checked)
            return checked;
        FloppyImportObject row{.key = object.key,
                               .name = object.object.header.name,
                               .display_name = object.object.header.name,
                               .type = object.object.header.type,
                               .size_bytes = object.raw_payload.size(),
                               .required_object_keys = {},
                               .exclusion_reason = {},
                               .sources = source_files.at(object.key)};
        if (const auto *program = std::get_if<CurrentProg>(&object.object.payload))
            row.display_name = program->program_name;
        const auto failed = std::ranges::find(catalog.issues, std::optional{object.sfs_id}, &CatalogIssue::sfs_id);
        if (failed != catalog.issues.end()) {
            row.exclusion_reason = failed->message;
        } else if (!root_kind(row.type)) {
            row.exclusion_reason = "This object type cannot be imported.";
        } else if (auto profile = package_internal::build_relocation_profile(object.object, object.raw_payload);
                   !profile) {
            row.exclusion_reason = profile.error().message;
        } else if (auto required = package_internal::required_relationships(object, graph, objects); !required) {
            row.exclusion_reason = required.error().message;
        } else {
            std::set<std::string> unique;
            for (const auto *edge : *required)
                if (edge->target_key)
                    unique.insert(*edge->target_key);
            row.required_object_keys.assign(unique.begin(), unique.end());
        }
        indices.emplace(row.key, inspection.objects.size());
        inspection.objects.push_back(std::move(row));
    }
    // A root is unavailable if any part of its required closure is unavailable.
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto &object : inspection.objects) {
            if (!object.exclusion_reason.empty())
                continue;
            for (const auto &key : object.required_object_keys) {
                const auto found = indices.find(key);
                if (found == indices.end() || !inspection.objects[found->second].exclusion_reason.empty()) {
                    object.exclusion_reason = "A required dependency cannot be imported.";
                    changed = true;
                    break;
                }
            }
        }
    }
    return {};
}

bool cataloged_object(std::string_view path, const std::optional<YamahaFloppyCatalog> &catalog) {
    const auto slot = detail::yamaha_floppy_filename_slot(path);
    if (!catalog || !slot)
        return false;
    const auto entry = std::ranges::find(catalog->files, *slot, &YamahaFloppyCatalogEntry::slot);
    if (entry == catalog->files.end())
        return false;
    for (const auto category : {R"(\SMPL\)", R"(\SBNK\)", R"(\SBAC\)", R"(\PROG\)", R"(\SEQU\)", R"(\PRF3\)"})
        if (entry->logical_path.starts_with(category))
            return true;
    return false;
}

Result<void> read_entry(std::vector<MediaObject> &objects, detail::FloppyFileSources &source_files,
                        FloppyImportInspection &inspection, std::size_t source_index, const std::string &member_name,
                        const std::string &path, std::vector<std::byte> bytes, bool cataloged, bool nested) {
    const bool recognized = detail::object_prefix(bytes);
    const bool supported_path = !nested || AxkObjectDirectory::recognizes_entry_prefix(bytes, true);
    if (!recognized || !supported_path) {
        const bool unreadable = cataloged || recognized;
        inspection.excluded_files.push_back(
            {member_name, path, bytes.size(),
             unreadable ? "Cataloged or recognized sampler object has no supported object header at this path."
                        : "Configuration or auxiliary file; not imported.",
             unreadable});
        return {};
    }
    auto decoded = detail::decode_media_object(bytes, bytes.size());
    if (!decoded) {
        inspection.excluded_files.push_back({member_name, path, bytes.size(), decoded.error().message, true});
        return {};
    }
    const auto scope = std::format("floppy-import-source:{}", source_index);
    const auto key = std::format("{}:{}", scope, path);
    source_files.emplace(key, std::vector<FloppyImportFileSource>{{member_name, path, bytes.size()}});
    objects.push_back({key,
                       path,
                       scope,
                       {},
                       scope,
                       {},
                       {member_name, LabelStatus::raw_identifier, "Selected source"},
                       0U,
                       bytes.size(),
                       std::move(decoded->object),
                       std::move(bytes),
                       std::move(decoded->issue)});
    return {};
}

bool inspect_set_completeness(FloppyImportInspection &inspection) {
    const auto &members = inspection.members;
    const bool ordinary = std::ranges::all_of(members, [](const auto &identity) {
        return !identity.trusted_for_disk_set &&
               (identity.marker == FloppySetMarker::none || identity.marker == FloppySetMarker::ordinary);
    });
    if (ordinary) {
        inspection.complete = true;
        return false;
    }
    const auto &first = members.front();
    std::map<std::uint16_t, FloppySetMarker> sequence;
    for (const auto &identity : members) {
        if (!identity.trusted_for_disk_set || identity.set_name != first.set_name ||
            !sequence.emplace(identity.index, identity.marker).second) {
            inspection.complete = false;
            return false;
        }
    }
    std::uint16_t next = 1U;
    while (sequence.contains(next))
        ++next;
    const bool contiguous = next == members.size() + 1U;
    const bool markers = std::ranges::all_of(sequence, [&](const auto &entry) {
        return entry.second ==
               (entry.first == sequence.rbegin()->first ? FloppySetMarker::final : FloppySetMarker::continuation);
    });
    inspection.complete = contiguous && markers;
    if (!inspection.complete)
        inspection.next_required_index = next;
    return inspection.complete;
}

Result<detail::RecoveredFloppyObjects> finish(std::vector<MediaObject> objects, detail::FloppyFileSources source_files,
                                              FloppyImportInspection &inspection,
                                              const CancellationToken &cancellation) {
    const bool trusted_complete = inspect_set_completeness(inspection);
    const auto &first = inspection.members.front();
    inspection.label = first.trusted_for_disk_set ? first.set_name : first.label;
    auto recovered = detail::recover_floppy_objects(std::move(objects), std::move(source_files), cancellation);
    if (!recovered)
        return std::unexpected(recovered.error());
    if (recovered->catalog.objects.empty() &&
        !std::ranges::any_of(inspection.excluded_files, &FloppyImportExcludedFile::unreadable_object))
        return std::unexpected(invalid("No A-series sampler objects were found in the selected sources."));
    if (auto checked = inspect_objects(inspection, recovered->catalog, recovered->relationships,
                                       recovered->source_files, cancellation);
        !checked)
        return std::unexpected(checked.error());
    inspection.can_import =
        std::ranges::any_of(inspection.objects, [](const auto &object) { return object.exclusion_reason.empty(); });
    const bool exclusions =
        std::ranges::any_of(inspection.objects, [](const auto &object) { return !object.exclusion_reason.empty(); }) ||
        std::ranges::any_of(inspection.excluded_files, &FloppyImportExcludedFile::unreadable_object);
    inspection.recovery_used =
        !trusted_complete && (recovered->joined || exclusions || !inspection.complete || !inspection.issues.empty());
    inspection.requires_acknowledgement =
        inspection.recovery_used || exclusions || !inspection.complete || !inspection.issues.empty();
    if (inspection.recovery_used)
        inspection.issues.push_back(
            {"FLOPPY_IMPORT_RECOVERY",
             "Only complete objects and their complete dependencies are available. Explicit-source recovery does not "
             "certify a disk set; unreadable or unresolved objects are excluded.",
             {},
             "Explicitly selected source bytes",
             "Review excluded objects before importing.",
             ValidationSeverity::warning});
    return recovered;
}
} // namespace

FloppyImportSource::FloppyImportSource(MediaKind kind, ObjectCatalog catalog, RelationshipGraph relationships,
                                       FloppyImportInspection inspection)
    : kind_(kind), catalog_(std::move(catalog)), relationships_(std::move(relationships)),
      inspection_(std::move(inspection)) {}

Result<FloppyImportSource> FloppyImportSource::open(std::vector<FatImage> members,
                                                    const CancellationToken &cancellation) {
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected(checked.error());
    if (members.empty() || members.size() > FloppyDiskSet::maximum_members)
        return std::unexpected(invalid("Choose between one and 32 floppy images."));
    FloppyImportInspection inspection;
    detail::FloppyFileSources source_files;
    std::vector<MediaObject> objects;
    std::size_t file_count{};
    for (std::size_t index = 0; index < members.size(); ++index) {
        const auto &member = members[index];
        if (member.geometry().profile != FatProfile::a_series_floppy ||
            member.geometry().physical_size_bytes > 4U * 1024U * 1024U)
            return std::unexpected(invalid("The source exceeds the A-series floppy image profile or size limit."));
        inspection.members.push_back(member.disk_identity());
        for (const auto &issue : member.validation_issues()) {
            inspection.issues.push_back(issue);
            inspection.issues.back().message = member.source_name() + ": " + issue.message;
        }
        std::uint64_t payload_bytes{};
        std::set<std::uint16_t> clusters;
        for (const auto &file : member.files()) {
            if (++file_count > 8192U || file.size > member.geometry().physical_size_bytes - payload_bytes)
                return std::unexpected(invalid("The floppy logical payload exceeds supported bounds."));
            payload_bytes += file.size;
            for (const auto cluster : file.clusters)
                if (!clusters.insert(cluster).second)
                    return std::unexpected(invalid("Floppy files have crosslinked cluster chains."));
            auto bytes = member.read_file(file, cancellation);
            if (!bytes)
                return std::unexpected(bytes.error());
            if (auto checked =
                    read_entry(objects, source_files, inspection, index, member.source_name(), file.path,
                               std::move(*bytes), cataloged_object(file.path, member.yamaha_catalog()), false);
                !checked)
                return std::unexpected(checked.error());
        }
    }
    auto recovered = finish(std::move(objects), std::move(source_files), inspection, cancellation);
    if (!recovered)
        return std::unexpected(recovered.error());
    return FloppyImportSource{MediaKind::fat12_floppy, std::move(recovered->catalog),
                              std::move(recovered->relationships), std::move(inspection)};
}

const FloppyImportInspection &FloppyImportSource::inspection() const noexcept { return inspection_; }

Result<FloppyImportSource> FloppyImportSource::open_directories(std::vector<FloppyImportDirectory> sources,
                                                                const CancellationToken &cancellation) {
    if (sources.empty() || sources.size() > FloppyDiskSet::maximum_members)
        return std::unexpected(invalid("Choose between one and 32 disk folders."));
    FloppyImportInspection inspection;
    detail::FloppyFileSources source_files;
    std::vector<MediaObject> objects;
    std::size_t count{};
    std::uint64_t total_bytes{};
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const auto &source = sources[index];
        for (const auto &entry : source.entries) {
            if (!entry.reader || ++count > AxkObjectDirectory::maximum_entries ||
                entry.reader->size() > AxkObjectDirectory::maximum_payload_bytes - total_bytes)
                return std::unexpected(invalid("The disk folders exceed the entry or payload limit."));
            total_bytes += entry.reader->size();
        }
        // Reuse directory path, depth, duplicate-name and leaf-size validation.
        if (auto checked = AxkObjectDirectory::recognizes(source.entries, source.name, cancellation); !checked)
            return std::unexpected(checked.error());
        std::vector<detail::FloppyCatalogFile> files;
        for (const auto &entry : source.entries)
            files.push_back({entry.name, entry.reader->size()});
        detail::FloppyCatalogInspection catalog;
        if (std::ranges::any_of(files,
                                [](const auto &file) { return detail::is_yamaha_floppy_catalog_path(file.path); })) {
            catalog = detail::inspect_yamaha_floppy_catalog(files, [&](std::size_t entry_index, std::size_t limit) {
                const auto &reader = *source.entries[entry_index].reader;
                return detail::read_bytes(
                    reader, 0U, static_cast<std::size_t>(std::min<std::uint64_t>(limit, reader.size())), cancellation);
            });
            for (const auto &issue : catalog.issues) {
                inspection.issues.push_back(issue);
                inspection.issues.back().message = source.name + ": " + issue.message;
            }
        }
        inspection.members.push_back(catalog.identity);
        for (const auto &entry : source.entries) {
            auto bytes =
                detail::read_bytes(*entry.reader, 0U, static_cast<std::size_t>(entry.reader->size()), cancellation);
            if (!bytes)
                return std::unexpected(bytes.error());
            if (auto checked = read_entry(objects, source_files, inspection, index, source.name, entry.name,
                                          std::move(*bytes), cataloged_object(entry.name, catalog.catalog),
                                          entry.name.find('/') != std::string::npos);
                !checked)
                return std::unexpected(checked.error());
        }
    }
    auto recovered = finish(std::move(objects), std::move(source_files), inspection, cancellation);
    if (!recovered)
        return std::unexpected(recovered.error());
    return FloppyImportSource{MediaKind::axk_object_directory, std::move(recovered->catalog),
                              std::move(recovered->relationships), std::move(inspection)};
}

Result<PortablePackage> FloppyImportSource::prepare(std::span<const std::string> selected_object_keys,
                                                    const CancellationToken &cancellation) const {
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected(checked.error());
    if (!inspection_.can_import)
        return std::unexpected(invalid("No complete objects are available to import."));
    if (selected_object_keys.empty())
        return std::unexpected(invalid("Select at least one object to import."));
    std::set<std::string> unique;
    std::vector<PackageRootSelector> roots;
    for (const auto &key : selected_object_keys) {
        const auto found = std::ranges::find(inspection_.objects, key, &FloppyImportObject::key);
        if (!unique.insert(key).second || found == inspection_.objects.end() || !found->exclusion_reason.empty())
            return std::unexpected(invalid("Selection contains an unknown, duplicate or excluded object."));
        PackageRootSelector root;
        root.kind = *root_kind(found->type);
        root.object_key = key;
        roots.push_back(std::move(root));
    }
    return package_internal::build_graph(kind_, catalog_, relationships_, roots, cancellation);
}
} // namespace axk
