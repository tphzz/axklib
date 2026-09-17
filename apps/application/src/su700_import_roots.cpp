#include "image_filesystem_internal.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <variant>

#include "axklib/su700.hpp"

namespace axk::app::detail {
void identify_su700_import_roots(ImageFilesystemIndex &index, const MediaContainer &media) {
    if (!std::holds_alternative<Container>(media.storage()) || index.device_view)
        return;
    std::map<std::pair<std::string, std::string>, const ImageFilesystemEntry *> paths;
    for (const auto &entry : index.entries)
        paths.emplace(std::pair{entry.root_id, entry.path}, &entry);
    for (const auto &entry : index.entries) {
        if (entry.name != "SONGCONT.DAT" || entry.size_bytes != 7400U || entry.ancestor_ids.size() != 2U ||
            entry.issue.size())
            continue;
        const auto file = index.files.find(entry.id);
        if (file == index.files.end())
            continue;
        const auto bytes = read_filesystem_range(media, file->second, 0, 7400U, {});
        if (!bytes)
            continue;
        const auto control = decode_su700_control(*bytes);
        if (!control || control->songs.empty())
            continue;
        const auto base = entry.path.substr(0, entry.path.find_last_of('/'));
        const auto has = [&](const std::string &path) {
            const auto found = paths.find({entry.root_id, path});
            return found != paths.end() && found->second->kind == "file" && found->second->issue.empty();
        };
        const bool resolved = std::ranges::all_of(control->songs, [&](const auto &song) {
            return has(base + "/SUSQ/" + song.name + ".SSQ") &&
                   std::ranges::all_of(song.samples,
                                       [&](const auto &sample) { return has(base + "/SUSP/" + sample + ".SSP"); });
        });
        if (!resolved)
            continue;
        for (auto &root : index.root_capabilities)
            if (root.root_id == entry.root_id && root.create_directory && root.put_file)
                root.supported_imports = {"SU700_FLOPPY"};
    }
}
} // namespace axk::app::detail
