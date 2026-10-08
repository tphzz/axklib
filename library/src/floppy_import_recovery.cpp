#include "floppy_import_recovery.hpp"

#include <algorithm>
#include <tuple>
#include <utility>

#include "axklib/package_relocation.hpp"
#include "media_internal.hpp"
#include "media_smpl_segments.hpp"
#include "package_internal.hpp"

namespace axk::detail {
namespace {
RelationshipGraph local_relationships(const std::vector<MediaObject> &objects) {
    std::vector<MediaObject> metadata;
    metadata.reserve(objects.size());
    for (const auto &object : objects) {
        MediaObject copy;
        copy.key = object.key;
        copy.scope_key = object.scope_key;
        copy.raw_volume = object.scope_key;
        copy.decoded.header = object.decoded.header;
        copy.decoded.format = object.decoded.format;
        if (!std::holds_alternative<GenericObject>(object.decoded.payload))
            copy.decoded.payload = object.decoded.payload;
        metadata.push_back(std::move(copy));
    }
    return build_relationship_graph(catalog_from_media_objects(std::move(metadata)));
}

void resolve_selected_dependencies(RecoveredFloppyObjects &result) {
    ObjectCatalog selected;
    for (const auto &object : result.catalog.objects) {
        const bool failed = std::ranges::any_of(result.catalog.issues,
                                                [&](const auto &issue) { return issue.sfs_id == object.sfs_id; });
        if (failed || !package_internal::build_relocation_profile(object.object, object.raw_payload))
            continue;
        DecodedObject metadata;
        metadata.header = object.object.header;
        metadata.format = object.object.format;
        if (!std::holds_alternative<GenericObject>(object.object.payload))
            metadata.payload = object.object.payload;
        ObjectSnapshot copy{object.key, object.partition, object.sfs_id, "explicit-floppy-selection",
                            std::move(metadata)};
        selected.objects.push_back(std::move(copy));
    }
    const auto candidates = build_relationship_graph(selected);
    using EdgeId = std::tuple<std::string, std::string, std::size_t>;
    std::map<std::pair<std::string, std::string>, std::size_t> ordinals;
    std::map<EdgeId, const Relationship *> indexed;
    for (const auto &edge : candidates.relationships) {
        if (!package_internal::closure_relationship(edge.type))
            continue;
        indexed.emplace(EdgeId{edge.source_key, edge.type, ordinals[{edge.source_key, edge.type}]++}, &edge);
    }
    ordinals.clear();
    for (auto &edge : result.relationships.relationships) {
        if (!package_internal::closure_relationship(edge.type))
            continue;
        const EdgeId id{edge.source_key, edge.type, ordinals[{edge.source_key, edge.type}]++};
        // Never replace a source-local target, including an unreadable one, or local name ambiguity.
        if (edge.target_key || (!edge.candidate_keys.empty() && edge.basis != "sbnk-member-cache-only-name-mismatch"))
            continue;
        const auto found = indexed.find(id);
        if (found == indexed.end() || found->second->quality != RelationshipQuality::known)
            continue;
        const auto scope = edge.scope_key;
        edge = *found->second;
        edge.scope_key = scope;
        edge.basis += "+explicit-selected-source";
        edge.notes = "Authoritative name and type resolve one decoded object in the explicitly selected sources.";
        result.joined = true;
    }
}
} // namespace

Result<RecoveredFloppyObjects> recover_floppy_objects(std::vector<MediaObject> objects, FloppyFileSources source_files,
                                                      const CancellationToken &cancellation) {
    RecoveredFloppyObjects result;
    result.relationships = local_relationships(objects);
    result.source_files = std::move(source_files);
    std::map<std::vector<std::byte>, std::vector<std::size_t>> groups;
    for (std::size_t index = 0; index < objects.size(); ++index) {
        const auto &object = objects[index];
        const auto &header = object.decoded.header;
        if (header.type != ObjectType::smpl ||
            (header.payload_offset_0x24 == 0U && header.payload_bytes_0x20 == header.payload_bytes_0x1c))
            continue;
        if (header.payload_bytes_0x1c > 64U * 1024U * 1024U)
            return std::unexpected(
                make_error(ErrorCode::io_unsupported_size, ErrorCategory::io, "Wave Data exceeds the assembly limit."));
        auto identity = smpl_segment_identity(object);
        if (!identity.empty())
            groups[std::move(identity)].push_back(index);
    }
    std::vector<bool> consumed(objects.size());
    std::map<std::string, std::string, std::less<>> aliases;
    for (auto &[identity, indices] : groups) {
        static_cast<void>(identity);
        if (indices.size() < 2U)
            continue;
        auto assembled = assemble_smpl_segment_group(objects, indices, cancellation);
        if (!assembled)
            return std::unexpected(assembled.error());
        if (!*assembled)
            continue;
        const auto &key = objects[indices.front()].key;
        for (std::size_t position = 1U; position < indices.size(); ++position) {
            const auto index = indices[position];
            consumed[index] = true;
            aliases.emplace(objects[index].key, key);
            auto &sources = result.source_files[key];
            const auto &other = result.source_files.at(objects[index].key);
            sources.insert(sources.end(), other.begin(), other.end());
        }
        result.joined = true;
    }
    std::vector<MediaObject> retained;
    for (std::size_t index = 0; index < objects.size(); ++index)
        if (!consumed[index])
            retained.push_back(std::move(objects[index]));
    result.catalog = catalog_from_media_objects(std::move(retained));
    for (auto &edge : result.relationships.relationships) {
        if (edge.target_key) {
            if (const auto found = aliases.find(*edge.target_key); found != aliases.end())
                edge.target_key = found->second;
        }
        for (auto &candidate : edge.candidate_keys)
            if (const auto found = aliases.find(candidate); found != aliases.end())
                candidate = found->second;
    }
    resolve_selected_dependencies(result);
    return result;
}
} // namespace axk::detail
