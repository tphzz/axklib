#include "server_application.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "authentication.hpp"
#include "axklib/server/job_json.hpp"
#include "axklib/server/telemetry.hpp"
#include "axklib/utf8.hpp"
#include "axklib/writer.hpp"
#include "download_reader.hpp"
#include "http_headers.hpp"
#include "route_registration.hpp"
#include "server_support.hpp"

namespace axk::server::detail {

crow::response ServerApplication::roots_response(const crow::request &request) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    Json roots = Json::array();
    static_cast<void>(workspaces_.snapshot());
    for (const auto &root : sandbox_.roots())
        roots.push_back({{"id", root.id}, {"displayName", root.display_name}, {"writable", root.writable}});
    return json_response(200, {{"data", {{"roots", std::move(roots)}}}, {"meta", {{"requestId", id}}}}, id);
}

Json ServerApplication::workspace_json(const axk::server::WorkspaceInfo &workspace) const {
    return {{"id", workspace.definition.id},
            {"displayName", workspace.definition.display_name},
            {"path", axk::text::path_to_utf8(workspace.definition.path)},
            {"writable", workspace.definition.writable},
            {"effectiveWritable", workspace.effective_writable},
            {"status", axk::server::workspace_status_name(workspace.status)},
            {"issue", workspace.issue ? Json(*workspace.issue) : Json{}}};
}

crow::response ServerApplication::workspace_snapshot_response(const crow::request &request) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto snapshot = workspaces_.snapshot();
    Json workspaces = Json::array();
    for (const auto &workspace : snapshot.workspaces)
        workspaces.push_back(workspace_json(workspace));
    return json_response(
        200,
        {{"data",
          {{"state", axk::server::workspace_configuration_state_name(snapshot.state)},
           {"revision", snapshot.revision},
           {"workspaces", std::move(workspaces)},
           {"configurationIssue", snapshot.configuration_issue ? Json(*snapshot.configuration_issue) : Json{}}}},
         {"meta", {{"requestId", id}}}},
        id);
}

crow::response ServerApplication::workspace_create_response(const crow::request &request) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto parsed = parse_validated_json_body(request, "WorkspaceCreateRequest");
    if (!parsed)
        return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);
    try {
        auto path = axk::text::path_from_utf8(parsed->at("path").get<std::string>());
        if (!path)
            return error_response(400, {"invalid_request", "workspace path is not valid UTF-8"}, id);
        auto added = workspaces_.add(parsed->at("displayName").get<std::string>(), std::move(*path),
                                     parsed->value("writable", true), parsed->at("revision").get<std::uint64_t>());
        if (!added)
            return error_response(added.error().code == "workspace_revision_conflict" ? 409 : 422, added.error(), id);
        static_cast<void>(workspaces_.snapshot());
        return json_response(201, {{"data", workspace_json(*added)}, {"meta", {{"requestId", id}}}}, id);
    } catch (const std::exception &) {
        return error_response(400, {"invalid_request", "workspace fields do not match the schema"}, id);
    }
}

crow::response ServerApplication::workspace_item_response(const crow::request &request,
                                                          const std::string &workspace_id) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto parsed = parse_validated_json_body(
        request, request.method == crow::HTTPMethod::Delete ? "RevisionRequest" : "WorkspaceUpdateRequest");
    if (!parsed)
        return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);
    try {
        const auto revision = parsed->at("revision").get<std::uint64_t>();
        auto reservation = path_reservations_.try_acquire(
            axk::app::PathAccess{{workspace_id, ""}, axk::app::PathAccessMode::exclusive});
        if (!reservation) {
            return error_response(
                409,
                {"workspace_in_use",
                 "close the open image or wait for active jobs using this workspace before changing it"},
                id);
        }
        if (request.method == crow::HTTPMethod::Delete) {
            auto removed = workspaces_.remove(workspace_id, revision);
            if (!removed)
                return error_response(removed.error().code == "workspace_revision_conflict" ? 409 : 404,
                                      removed.error(), id);
            static_cast<void>(workspaces_.snapshot());
            return crow::response{204};
        }
        std::optional<std::string> display_name;
        std::optional<std::filesystem::path> path;
        std::optional<bool> writable;
        if (const auto found = parsed->find("displayName"); found != parsed->end())
            display_name = found->get<std::string>();
        if (const auto found = parsed->find("path"); found != parsed->end()) {
            auto parsed_path = axk::text::path_from_utf8(found->get<std::string>());
            if (!parsed_path)
                return error_response(400, {"invalid_request", "workspace path is not valid UTF-8"}, id);
            path = std::move(*parsed_path);
        }
        if (const auto found = parsed->find("writable"); found != parsed->end())
            writable = found->get<bool>();
        auto updated = workspaces_.update(workspace_id, std::move(display_name), std::move(path), writable, revision);
        if (!updated)
            return error_response(updated.error().code == "workspace_revision_conflict" ? 409 : 422, updated.error(),
                                  id);
        static_cast<void>(workspaces_.snapshot());
        return json_response(200, {{"data", workspace_json(*updated)}, {"meta", {{"requestId", id}}}}, id);
    } catch (const std::exception &) {
        return error_response(400, {"invalid_request", "workspace fields do not match the schema"}, id);
    }
}

crow::response ServerApplication::workspace_reset_response(const crow::request &request) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    auto reservation = path_reservations_.try_acquire(
        axk::app::PathAccess{.reference = {}, .mode = axk::app::PathAccessMode::exclusive, .all_roots = true});
    if (!reservation) {
        return error_response(
            409, {"workspace_in_use", "close images and wait for active jobs before resetting workspaces"}, id);
    }
    auto reset = workspaces_.archive_and_reset();
    if (!reset)
        return error_response(500, reset.error(), id);
    return json_response(
        200,
        {{"data", {{"archivedPath", *reset ? Json(axk::text::path_to_utf8(**reset)) : Json{}}, {"revision", 0U}}},
         {"meta", {{"requestId", id}}}},
        id);
}

crow::response ServerApplication::host_directory_roots_response(const crow::request &request) const {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    Json roots = Json::array();
#if defined(_WIN32)
    const auto drives = GetLogicalDrives();
    for (unsigned int index = 0U; index < 26U; ++index) {
        if ((drives & (1UL << index)) == 0U)
            continue;
        const auto letter = static_cast<char>('A' + index);
        const auto path = std::string{letter} + ":/";
        roots.push_back({{"name", path}, {"path", path}});
    }
#else
    roots.push_back({{"name", "Filesystem"}, {"path", "/"}});
#endif
    return json_response(200, {{"data", {{"roots", std::move(roots)}}}, {"meta", {{"requestId", id}}}}, id);
}

crow::response ServerApplication::host_directory_listing_response(const crow::request &request) const {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto parsed = parse_validated_json_body(request, "HostDirectoryListRequest");
    if (!parsed)
        return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);
    try {
        auto parsed_path = axk::text::path_from_utf8(parsed->at("path").get<std::string>());
        if (!parsed_path)
            return error_response(400, {"invalid_request", "host directory path is not valid UTF-8"}, id);
        auto path = std::move(*parsed_path);
        const auto limit = static_cast<std::size_t>(std::min(parsed->value("limit", 200U), 500U));
        const auto offset = parsed->value("cursor", std::string{});
        std::size_t first{};
        if (!offset.empty()) {
            const auto [tail, error] = std::from_chars(offset.data(), offset.data() + offset.size(), first);
            if (error != std::errc{} || tail != offset.data() + offset.size())
                return error_response(400, {"invalid_cursor", "directory cursor is invalid"}, id);
        }
        if (!path.is_absolute())
            return error_response(422, {"invalid_host_directory", "host directory path must be absolute"}, id);
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
            return error_response(422, {"invalid_host_directory", "host directory is inaccessible"}, id);
        path = std::filesystem::canonical(path, error);
        if (error)
            return error_response(422, {"invalid_host_directory", "host directory is inaccessible"}, id);
        std::vector<std::filesystem::path> directories;
        for (std::filesystem::directory_iterator
                 iterator{path, std::filesystem::directory_options::skip_permission_denied, error},
             end;
             !error && iterator != end; iterator.increment(error)) {
            std::error_code entry_error;
            const auto entry_status = iterator->symlink_status(entry_error);
            if (!entry_error && !std::filesystem::is_symlink(entry_status) &&
                std::filesystem::is_directory(entry_status)) {
                directories.push_back(iterator->path());
            }
        }
        if (error)
            return error_response(422, {"invalid_host_directory", "host directory cannot be listed"}, id);
        std::ranges::sort(directories, {}, [](const auto &entry) { return axk::text::path_to_utf8(entry.filename()); });
        if (first > directories.size())
            return error_response(400, {"invalid_cursor", "directory cursor is outside the listing"}, id);
        Json entries = Json::array();
        const auto end = first + std::min(limit, directories.size() - first);
        for (auto index = first; index < end; ++index) {
            entries.push_back({{"name", axk::text::path_to_utf8(directories[index].filename())},
                               {"path", axk::text::path_to_utf8(directories[index])}});
        }
        return json_response(200,
                             {{"data",
                               {{"path", axk::text::path_to_utf8(path)},
                                {"parentPath", path.has_parent_path() && path != path.root_path()
                                                   ? Json(axk::text::path_to_utf8(path.parent_path()))
                                                   : Json{}},
                                {"entries", std::move(entries)},
                                {"nextCursor", end < directories.size() ? Json(std::to_string(end)) : Json{}}}},
                              {"meta", {{"requestId", id}}}},
                             id);
    } catch (const std::exception &) {
        return error_response(400, {"invalid_request", "host directory fields do not match the schema"}, id);
    }
}

crow::response ServerApplication::directory_listing_response(const crow::request &request) const {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto parsed = parse_validated_json_body(request, "DirectoryListRequest");
    if (!parsed)
        return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);
    const auto &input = *parsed;

    axk::app::DirectoryRef directory;
    std::size_t limit = 200U;
    std::optional<std::string> cursor;
    try {
        const auto &reference = input.at("directory");
        directory.root_id = reference.at("rootId").get<std::string>();
        directory.relative_path = reference.at("relativePath").get<std::string>();
        if (const auto found = input.find("limit"); found != input.end())
            limit = found->get<std::size_t>();
        if (const auto found = input.find("cursor"); found != input.end() && !found->is_null())
            cursor = found->get<std::string>();
    } catch (const Json::exception &) {
        return error_response(400, {"invalid_request", "directory listing fields do not match the schema"}, id);
    }
    if (cursor && (cursor->empty() || cursor->size() > maximum_cursor_length))
        return error_response(400, {"invalid_cursor", "cursor length is outside the configured contract"}, id);
    if (limit > config_.maximum_page_size)
        return error_response(422, {"invalid_page_size", "directory listing limit exceeds server capabilities"}, id);
    const auto started = std::chrono::steady_clock::now();
    const auto listing = sandbox_.list_directory(directory, limit, cursor);
    if (!listing)
        return error_response(422, listing.error(), id);
    Json entries = Json::array();
    for (const auto &entry : listing->entries) {
        entries.push_back({{"name", entry.name},
                           {"relativePath", entry.relative_path},
                           {"kind", axk::app::directory_entry_kind_name(entry.kind)},
                           {"size", entry.size ? Json(*entry.size) : Json{}}});
    }
    if (auto diagnostic = operation_diagnostic_sink()) {
        diagnostic({{"event", "filesystem_directory_list"},
                    {"durationMs",
                     std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                         .count()},
                    {"entryCount", listing->entries.size()},
                    {"truncated", listing->truncated}});
    }
    return json_response(
        200,
        {{"data",
          {{"directory", {{"rootId", listing->directory.root_id}, {"relativePath", listing->directory.relative_path}}},
           {"entries", std::move(entries)},
           {"truncated", listing->truncated},
           {"nextCursor", listing->next_cursor ? Json(*listing->next_cursor) : Json{}}}},
         {"meta", {{"requestId", id}}}},
        id);
}

crow::response ServerApplication::media_source_inspection_response(const crow::request &request) const {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto parsed = parse_validated_json_body(request, "MediaSourceInspectRequest");
    if (!parsed)
        return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);
    axk::app::DirectoryRef directory;
    try {
        const auto &reference = parsed->at("directory");
        directory.root_id = reference.at("rootId").get<std::string>();
        directory.relative_path = reference.at("relativePath").get<std::string>();
    } catch (const Json::exception &) {
        return error_response(400, {"invalid_request", "media-source fields do not match the schema"}, id);
    }
    const auto started = std::chrono::steady_clock::now();
    const auto inspection = sandbox_.inspect_media_source(directory);
    if (!inspection)
        return error_response(422, inspection.error(), id);
    if (auto diagnostic = operation_diagnostic_sink()) {
        diagnostic({{"event", "filesystem_media_source_inspect"},
                    {"durationMs",
                     std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                         .count()},
                    {"entriesVisited", inspection->entries_visited},
                    {"prefixesRead", inspection->prefixes_read},
                    {"recognized", inspection->kind.has_value()}});
    }
    return json_response(
        200,
        {{"data",
          {{"mediaSourceKind",
            inspection->kind ? Json(axk::app::directory_media_source_kind_name(*inspection->kind)) : Json{}}}},
         {"meta", {{"requestId", id}}}},
        id);
}

crow::response ServerApplication::metadata_response(const crow::request &request) const {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto parsed = parse_validated_json_body(request, "EntryRef");
    if (!parsed)
        return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);
    const auto &input = *parsed;
    std::string root_id;
    std::string relative_path;
    try {
        root_id = input.at("rootId").get<std::string>();
        relative_path = input.at("relativePath").get<std::string>();
    } catch (const Json::exception &) {
        return error_response(400, {"invalid_request", "rootId and relativePath are required strings"}, id);
    }
    const auto metadata = sandbox_.metadata(root_id, relative_path);
    if (!metadata)
        return error_response(422, metadata.error(), id);
    return json_response(200,
                         {{"data",
                           {{"rootId", metadata->root_id},
                            {"relativePath", metadata->relative_path},
                            {"kind", axk::app::directory_entry_kind_name(metadata->kind)},
                            {"size", metadata->size ? Json(*metadata->size) : Json{}},
                            {"writable", metadata->writable}}},
                          {"meta", {{"requestId", id}}}},
                         id);
}

Json ServerApplication::entry_metadata_json(const axk::app::EntryMetadata &metadata) const {
    return {{"rootId", metadata.root_id},
            {"relativePath", metadata.relative_path},
            {"kind", axk::app::directory_entry_kind_name(metadata.kind)},
            {"size", metadata.size ? Json(*metadata.size) : Json{}},
            {"writable", metadata.writable}};
}

crow::response ServerApplication::create_directory_response(const crow::request &request) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto parsed = parse_validated_json_body(request, "CreateDirectoryRequest");
    if (!parsed)
        return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);
    axk::app::DirectoryRef parent;
    std::string name;
    try {
        const auto &reference = parsed->at("parent");
        parent = {reference.at("rootId").get<std::string>(), reference.at("relativePath").get<std::string>()};
        name = parsed->at("name").get<std::string>();
    } catch (const Json::exception &) {
        return error_response(400, {"invalid_request", "parent and name do not match the schema"}, id);
    }
    const auto relative_path = parent.relative_path.empty() ? name : parent.relative_path + '/' + name;
    auto reservation = path_reservations_.try_acquire(
        axk::app::PathAccess{{parent.root_id, relative_path}, axk::app::PathAccessMode::exclusive});
    if (!reservation)
        return error_response(409, {"entry_in_use", "wait for active image and file operations to finish"}, id);
    const auto created = sandbox_.create_directory(parent, name);
    if (!created)
        return error_response(status_for_error(created.error()), created.error(), id);
    return json_response(201, {{"data", entry_metadata_json(*created)}, {"meta", {{"requestId", id}}}}, id);
}

crow::response ServerApplication::rename_entry_response(const crow::request &request) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto parsed = parse_validated_json_body(request, "RenameEntryRequest");
    if (!parsed)
        return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);
    axk::app::FileRef entry;
    std::string name;
    try {
        const auto &reference = parsed->at("entry");
        entry = {reference.at("rootId").get<std::string>(), reference.at("relativePath").get<std::string>()};
        name = parsed->at("name").get<std::string>();
    } catch (const Json::exception &) {
        return error_response(400, {"invalid_request", "entry and name do not match the schema"}, id);
    }
    const auto parent = std::filesystem::path{entry.relative_path}.parent_path().generic_string();
    const axk::app::FileRef destination{entry.root_id, parent.empty() ? name : parent + '/' + name};
    const std::array accesses{
        axk::app::PathAccess{entry, axk::app::PathAccessMode::exclusive},
        axk::app::PathAccess{destination, axk::app::PathAccessMode::exclusive},
    };
    auto reservation = path_reservations_.try_acquire(accesses);
    if (!reservation)
        return error_response(409, {"entry_in_use", "close open images and wait for active jobs before renaming"}, id);
    const auto renamed = sandbox_.rename_entry(entry, name);
    if (!renamed)
        return error_response(status_for_error(renamed.error()), renamed.error(), id);
    return json_response(200, {{"data", entry_metadata_json(*renamed)}, {"meta", {{"requestId", id}}}}, id);
}

crow::response ServerApplication::delete_entry_response(const crow::request &request) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto *root_id = request.url_params.get("rootId");
    const auto *relative_path = request.url_params.get("relativePath");
    if (root_id == nullptr || relative_path == nullptr)
        return error_response(400, {"invalid_request", "rootId and relativePath query parameters are required"}, id);
    const axk::app::FileRef entry{root_id, relative_path};
    auto reservation = path_reservations_.try_acquire(axk::app::PathAccess{entry, axk::app::PathAccessMode::exclusive});
    if (!reservation)
        return error_response(409, {"entry_in_use", "close open images and wait for active jobs before deleting"}, id);
    const auto deleted = sandbox_.delete_entry(entry);
    if (!deleted)
        return error_response(status_for_error(deleted.error()), deleted.error(), id);
    return json_response(200, {{"data", {{"deleted", true}}}, {"meta", {{"requestId", id}}}}, id);
}

} // namespace axk::server::detail
