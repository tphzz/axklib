#include "axklib/floppy_import.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "axklib/package_closure.hpp"
#include "axklib/package_graph.hpp"
#include "axklib/package_relocation.hpp"
#include "axklib/relationship.hpp"

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
                             const CancellationToken &cancellation) {
    const auto graph = build_relationship_graph(catalog);
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
                               .exclusion_reason = {}};
        if (const auto *program = std::get_if<CurrentProg>(&object.object.payload))
            row.display_name = program->program_name;
        if (!root_kind(row.type)) {
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

Result<void> excluded_files(FloppyImportInspection &inspection, const FatImage &member,
                            const CancellationToken &cancellation) {
    auto objects = member.objects(MediaObjectReadMode::decoded_metadata, 64U * 1024U * 1024U, cancellation);
    if (!objects)
        return std::unexpected(objects.error());
    std::set<std::string> paths;
    for (const auto &object : *objects)
        paths.insert(object.logical_path);
    for (const auto &file : member.files()) {
        if (!paths.contains(file.path))
            inspection.excluded_files.push_back({member.source_name(), file.path, file.size});
    }
    return {};
}
} // namespace

FloppyImportSource::FloppyImportSource(MediaKind kind, ObjectCatalog catalog, FloppyImportInspection inspection)
    : kind_(kind), catalog_(std::move(catalog)), inspection_(std::move(inspection)) {}

Result<FloppyImportSource> FloppyImportSource::open(std::vector<FatImage> members,
                                                    const CancellationToken &cancellation) {
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected(checked.error());
    if (members.empty() || members.size() > FloppyDiskSet::maximum_members)
        return std::unexpected(invalid("Choose one floppy or a set of at most 32 members."));
    FloppyImportInspection inspection;
    std::size_t file_count{};
    for (const auto &member : members) {
        if (member.geometry().physical_size_bytes > 4U * 1024U * 1024U)
            return std::unexpected(invalid("The source exceeds the floppy image size limit."));
        std::uint64_t payload_bytes{};
        for (const auto &file : member.files()) {
            if (++file_count > 8192U || file.size > member.geometry().physical_size_bytes - payload_bytes)
                return std::unexpected(invalid("The floppy logical payload exceeds supported bounds."));
            payload_bytes += file.size;
        }
        const auto &identity = member.disk_identity();
        if (!identity.trusted_for_disk_set &&
            (identity.marker == FloppySetMarker::continuation || identity.marker == FloppySetMarker::final ||
             identity.marker == FloppySetMarker::invalid || identity.index > 1U)) {
            return std::unexpected(invalid("This disk has unsupported or inconsistent disk-set metadata."));
        }
        inspection.members.push_back(member.disk_identity());
        if (auto excluded = excluded_files(inspection, member, cancellation); !excluded)
            return std::unexpected(excluded.error());
    }
    const auto first = members.front().disk_identity();
    inspection.label = first.trusted_for_disk_set ? first.set_name : first.label;
    std::optional<MediaContainer> media;
    if (members.size() > 1U) {
        auto set = FloppyDiskSet::open(std::move(members), {}, cancellation);
        if (!set)
            return std::unexpected(set.error());
        inspection.complete = set->status() == FloppySetStatus::complete;
        inspection.next_required_index = set->next_required_index();
        std::ranges::sort(inspection.members, {}, &FloppyDiskIdentity::index);
        media.emplace(std::move(*set));
    } else {
        inspection.complete =
            !first.trusted_for_disk_set || (first.marker == FloppySetMarker::final && first.index == 1U);
        if (first.trusted_for_disk_set && !inspection.complete)
            inspection.next_required_index = first.index == 1U ? 2U : 1U;
        media.emplace(std::move(members.front()));
    }
    const auto issues = media->validation_issues();
    inspection.issues.assign(issues.begin(), issues.end());
    if (!inspection.complete)
        return FloppyImportSource{media->kind(), {}, std::move(inspection)};
    auto catalog = build_object_catalog(*media, 64U * 1024U * 1024U, cancellation);
    if (!catalog)
        return std::unexpected(catalog.error());
    if (catalog->objects.empty())
        return std::unexpected(invalid("No A-series sampler objects were found in this floppy."));
    if (auto inspected = inspect_objects(inspection, *catalog, cancellation); !inspected)
        return std::unexpected(inspected.error());
    return FloppyImportSource{media->kind(), std::move(*catalog), std::move(inspection)};
}

const FloppyImportInspection &FloppyImportSource::inspection() const noexcept { return inspection_; }

Result<PortablePackage> FloppyImportSource::prepare(std::span<const std::string> selected_object_keys,
                                                    const CancellationToken &cancellation) const {
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected(checked.error());
    if (!inspection_.complete)
        return std::unexpected(invalid("Add the missing companion disks before importing."));
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
    return package_internal::build_graph(kind_, catalog_, roots, cancellation);
}
} // namespace axk
