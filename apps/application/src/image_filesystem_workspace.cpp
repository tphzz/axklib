#include "image_sessions_internal.hpp"

#include "axklib/utf8.hpp"
#include <filesystem>
#include <map>
#include <utility>

namespace axk::app {
namespace {

detail::ImageFilesystemIndex plain_files(const ImageSourceMember &member) {
    detail::ImageFilesystemIndex index;
    index.available = true;
    index.filesystem_name = member.format;
    for (const auto &root : member.roots) {
        ImageFilesystemEntry parent{};
        parent.id = parent.root_id = root.id;
        parent.name = text::path_to_utf8(member.canonical_path.filename());
        parent.path = "/";
        parent.kind = "directory";
        index.entries.push_back(std::move(parent));
        for (const auto &item : root.entries) {
            ImageFilesystemEntry entry{};
            entry.id = "entry:" + item.id;
            entry.root_id = root.id;
            const auto path = text::path_from_utf8(item.path).value();
            entry.path = item.path;
            entry.name = text::path_to_utf8(path.filename());
            const auto ancestor = text::path_to_utf8(path.parent_path());
            entry.parent_id = member.format == "directory" && !ancestor.empty() ? "entry:" + ancestor : root.id;
            entry.kind = item.directory ? "directory" : "file";
            if (item.reader)
                entry.size_bytes = item.reader->size();
            index.entries.push_back(std::move(entry));
        }
    }
    std::map<std::string, ImageFilesystemEntry *> entries;
    for (auto &entry : index.entries)
        entries.emplace(entry.id, &entry);
    for (auto &entry : index.entries) {
        if (entry.parent_id)
            ++entries.at(*entry.parent_id)->child_count;
        auto parent = entry.parent_id;
        while (parent) {
            entry.ancestor_ids.push_back(*parent);
            parent = entries.at(*parent)->parent_id;
        }
        std::ranges::reverse(entry.ancestor_ids);
    }
    return index;
}

} // namespace

Result<std::shared_ptr<const detail::ImageFilesystemIndex>>
ImageSessionManager::Implementation::filesystem(Session &session) const {
    if (session.filesystem_index)
        return session.filesystem_index;
    auto result = std::make_shared<detail::ImageFilesystemIndex>();
    result->available = true;
    result->filesystem_name =
        session.sources.members.size() == 1U ? session.sources.members.front()->format : "sources";
    for (const auto &member : session.sources.members) {
        auto local = member->media ? detail::build_image_filesystem(*member->media, member->reader, {})
                                   : Result<detail::ImageFilesystemIndex>{plain_files(*member)};
        if (!local)
            return std::unexpected(local.error());
        const bool plain = !member->media || !local->available;
        if (!local->available)
            *local = plain_files(*member);
        if (session.sources.members.size() == 1U)
            result->filesystem_name = local->filesystem_name;
        std::map<std::string, ImageFilesystemEntry *> entries;
        for (auto &entry : local->entries)
            entries.emplace(entry.id, &entry);
        using Location = std::pair<std::string, std::string>;
        std::map<Location, std::vector<ImageFilesystemBinding>> bindings;
        std::map<Location, std::shared_ptr<const RandomAccessReader>> readers;
        for (const auto &root : member->roots)
            for (const auto &entry : root.entries)
                if (entry.reader)
                    readers.emplace(Location{root.id, entry.id}, entry.reader);
        for (const auto &document : session.documents) {
            for (const auto &object : document->native->native_document->catalog().objects) {
                const auto id = document->native->native_object_ids.find(object.id);
                if (id == document->native->native_object_ids.end())
                    continue;
                for (const auto &location : object.locations)
                    if (location.member_id == member->id)
                        bindings[{location.root_id, location.entry_id}].push_back(
                            {document->id, id->second, std::nullopt});
            }
            if (document->primary != member || !member->media || document->member_ids.size() != 1U)
                continue;
            auto bound =
                detail::build_image_filesystem(*member->media, member->reader, document->native->filesystem_bindings());
            if (!bound)
                return std::unexpected(bound.error());
            for (const auto &binding : bound->entries) {
                auto entry = entries.find(binding.id);
                if (entry != entries.end() && (binding.object_id || binding.content_scope_id)) {
                    entry->second->bindings.push_back({document->id, binding.object_id, binding.content_scope_id});
                    entry->second->interpretation = binding.interpretation;
                }
            }
        }
        const auto prefix = member->id + ":";
        for (auto &entry : local->entries) {
            const auto local_id = plain && entry.parent_id ? entry.id.substr(6U) : entry.id;
            const Location location{entry.root_id, local_id};
            if (const auto found = bindings.find(location); found != bindings.end())
                for (const auto &binding : found->second)
                    if (!std::ranges::any_of(entry.bindings, [&](const auto &existing) {
                            return existing.document_id == binding.document_id &&
                                   existing.object_id == binding.object_id;
                        }))
                        entry.bindings.push_back(binding);
            if (const auto file = readers.find(location); file != readers.end())
                result->files.emplace(prefix + entry.id, detail::ReaderFilesystemFile{file->second});
            if (entry.parent_id)
                entry.parent_id = prefix + *entry.parent_id;
            else if (session.sources.members.size() > 1U)
                entry.name = text::path_to_utf8(member->canonical_path.filename()) + " / " + entry.name;
            entry.id = prefix + entry.id;
            entry.root_id = prefix + entry.root_id;
            for (auto &ancestor : entry.ancestor_ids)
                ancestor = prefix + ancestor;
            entry.source_member_id = member->id;
            result->entries.push_back(std::move(entry));
        }
        for (const auto &[root, partition] : local->edit_partitions)
            result->edit_partitions.emplace(prefix + root, partition);
        for (auto capability : local->root_capabilities) {
            capability.root_id = prefix + capability.root_id;
            auto metadata = sandbox.metadata(member->reference.root_id, member->reference.relative_path);
            if (!path_reservations || !metadata || !metadata->writable ||
                member->reference.kind != ImageSourceKind::file) {
                capability.create_directory = capability.put_file = capability.delete_entry = capability.rename_entry =
                    capability.move_entry = false;
                capability.supported_imports.clear();
            }
            result->root_capabilities.push_back(std::move(capability));
        }
    }
    session.filesystem_index = result;
    return session.filesystem_index;
}

Result<ImageSessionRead> ImageSessionManager::begin_filesystem_read(std::string_view image_id,
                                                                    std::string_view owner_id,
                                                                    std::uint64_t expected_revision) {
    auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    auto access = std::unique_lock{(*session)->access_mutex};
    if ((*session)->revision != expected_revision)
        return std::unexpected(session_error("image_revision_stale", "Image session revision changed", true));
    if ((*session)->mutating || (*session)->invalidated)
        return std::unexpected(session_error("entry_in_use", "Image session is unavailable", true));
    auto verify = [sources = (*session)->sources.native_sources]() -> Result<void> {
        auto checked = sources->verify();
        if (!checked)
            return std::unexpected(session_error("image_source_changed", checked.error().message));
        return {};
    };
    if (auto checked = verify(); !checked)
        return std::unexpected(checked.error());
    auto index = implementation_->filesystem(**session);
    if (!index)
        return std::unexpected(index.error());
    struct Lease {
        std::shared_ptr<Implementation::Session> owner;
        std::unique_lock<std::mutex> lock;
    };
    ImageSessionRead read{};
    read.image_id = (*session)->image_id;
    read.revision = (*session)->revision;
    read.lease = std::make_shared<Lease>(Lease{*session, std::move(access)});
    read.verify_source_unchanged = std::move(verify);
    read.filesystem = *index;
    return read;
}

namespace detail {
Result<std::vector<std::byte>> read_workspace_file(const FilesystemFileLocator &file, std::uint64_t offset,
                                                   std::size_t size, const CancellationToken &cancellation) {
    if (auto active = cancellation.check(); !active)
        return std::unexpected(session_error("operation_cancelled", active.error().message));
    const auto *source = std::get_if<ReaderFilesystemFile>(&file);
    if (!source || !source->reader || offset > source->reader->size() || size > source->reader->size() - offset)
        return std::unexpected(session_error("invalid_file_range", "File range exceeds its source"));
    std::vector<std::byte> bytes(size);
    if (auto read = source->reader->read_exact_at(offset, bytes); !read)
        return std::unexpected(session_error("filesystem_read_failed", read.error().message));
    return bytes;
}
} // namespace detail
} // namespace axk::app
