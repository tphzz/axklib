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

crow::response ServerApplication::create_upload_response(const crow::request &request) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto parsed = parse_validated_json_body(request, "UploadCreateRequest");
    if (!parsed)
        return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);
    const auto &input = *parsed;
    axk::app::UploadCreateRequest create_request;
    try {
        create_request.owner_id = request_owner(request);
        create_request.filename = input.at("filename").get<std::string>();
        const auto kind = parse_upload_kind(input.at("kind").get<std::string>());
        if (!kind)
            return error_response(400, {"invalid_request", "upload kind must be audio, package, or manifest"}, id);
        create_request.kind = *kind;
        create_request.media_type = input.at("mediaType").get<std::string>();
        create_request.declared_size = input.at("size").get<std::uint64_t>();
        if (const auto digest = input.find("sha256"); digest != input.end() && !digest->is_null())
            create_request.sha256 = digest->get<std::string>();
    } catch (const Json::exception &) {
        return error_response(400, {"invalid_request", "upload metadata does not match the schema"}, id);
    }
    const auto upload = uploads_.create(std::move(create_request));
    if (!upload) {
        audit(id, "upload_create", "denied", request_owner(request));
        return error_response(status_for_error(upload.error()), upload.error(), id);
    }
    audit(id, "upload_create", "allowed", request_owner(request), "upload", upload->reference.upload_id);
    auto response = json_response(201, {{"data", upload_json(*upload)}, {"meta", {{"requestId", id}}}}, id);
    response.set_header("Location", "/api/v1/uploads/" + upload->reference.upload_id);
    response.set_header("Upload-Offset", "0");
    return response;
}

crow::response ServerApplication::upload_response(const crow::request &request, const std::string &upload_id) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const axk::app::UploadRef reference{upload_id};
    if (request.method == crow::HTTPMethod::Delete) {
        const auto removed = uploads_.remove(reference, request_owner(request));
        if (!removed) {
            audit(id, "upload_delete", "denied", request_owner(request), "upload", upload_id);
            return error_response(status_for_error(removed.error()), removed.error(), id);
        }
        audit(id, "upload_delete", "allowed", request_owner(request), "upload", upload_id);
        return crow::response{204};
    }
    if (request.method == crow::HTTPMethod::Put) {
        const auto offset = parse_unsigned(request.get_header_value("Upload-Offset"));
        if (!offset)
            return error_response(400, {"invalid_upload_chunk", "Upload-Offset must be an unsigned integer"}, id);
        if (request.body.size() > uploads_.maximum_chunk_bytes())
            return error_response(413, {"request_too_large", "upload chunk exceeds the configured limit"}, id);
        const auto body = std::as_bytes(std::span{request.body.data(), request.body.size()});
        const auto upload = uploads_.append(reference, request_owner(request), *offset, body);
        if (!upload) {
            audit(id, "upload_append", "denied", request_owner(request), "upload", upload_id);
            return error_response(status_for_error(upload.error()), upload.error(), id);
        }
        audit(id, "upload_append", "allowed", request_owner(request), "upload", upload_id);
        auto response = json_response(200, {{"data", upload_json(*upload)}, {"meta", {{"requestId", id}}}}, id);
        response.set_header("Upload-Offset", std::to_string(upload->received_size));
        return response;
    }
    const auto upload = uploads_.inspect(reference, request_owner(request));
    if (!upload)
        return error_response(status_for_error(upload.error()), upload.error(), id);
    auto response = json_response(200, {{"data", upload_json(*upload)}, {"meta", {{"requestId", id}}}}, id);
    response.set_header("Upload-Offset", std::to_string(upload->received_size));
    return response;
}

crow::response ServerApplication::complete_upload_response(const crow::request &request, const std::string &upload_id) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto upload = uploads_.complete({upload_id}, request_owner(request));
    if (!upload) {
        audit(id, "upload_complete", "denied", request_owner(request), "upload", upload_id);
        return error_response(status_for_error(upload.error()), upload.error(), id);
    }
    audit(id, "upload_complete", "allowed", request_owner(request), "upload", upload_id);
    return json_response(200, {{"data", upload_json(*upload)}, {"meta", {{"requestId", id}}}}, id);
}

crow::response ServerApplication::materialize_upload_response(const crow::request &request,
                                                              const std::string &upload_id) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto parsed = parse_validated_json_body(request, "UploadMaterializeRequest");
    if (!parsed)
        return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);
    const auto &input = *parsed;
    axk::app::FileRef destination;
    bool overwrite{};
    try {
        const auto &reference = input.at("destination");
        destination.root_id = reference.at("rootId").get<std::string>();
        destination.relative_path = reference.at("relativePath").get<std::string>();
        overwrite = input.value("overwrite", false);
    } catch (const Json::exception &) {
        return error_response(400, {"invalid_request", "destination must be one sandbox FileRef"}, id);
    }
    auto reservation =
        path_reservations_.try_acquire(axk::app::PathAccess{destination, axk::app::PathAccessMode::exclusive});
    if (!reservation)
        return error_response(409, reservation.error(), id);
    const auto materialized =
        uploads_.materialize({upload_id}, request_owner(request), sandbox_, destination, overwrite);
    if (!materialized) {
        audit(id, "upload_materialize", "denied", request_owner(request), "upload", upload_id);
        return error_response(status_for_error(materialized.error()), materialized.error(), id);
    }
    audit(id, "upload_materialize", "allowed", request_owner(request), "upload", upload_id);
    return json_response(
        201,
        {{"data", {{"file", {{"rootId", materialized->root_id}, {"relativePath", materialized->relative_path}}}}},
         {"meta", {{"requestId", id}}}},
        id);
}

crow::response ServerApplication::download_response(const crow::request &request) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto *root_id = request.url_params.get("rootId");
    const auto *relative_path = request.url_params.get("relativePath");
    if (root_id == nullptr || relative_path == nullptr)
        return error_response(400, {"invalid_request", "rootId and relativePath query parameters are required"}, id);
    const axk::app::FileRef reference{root_id, relative_path};
    auto reservation = path_reservations_.try_acquire({reference, axk::app::PathAccessMode::shared});
    if (!reservation)
        return error_response(409, reservation.error(), id);
    const auto file = sandbox_.open_file(reference);
    if (!file) {
        audit(id, "file_download", "denied", request_owner(request), "root", root_id);
        return error_response(404, file.error(), id);
    }
    const auto size = file->size;
    const auto revision = '"' + file->revision + '"';

    crow::response response;
    const auto set_common_headers = [&] {
        response.set_header("Content-Type", "application/octet-stream");
        response.set_header("X-Request-Id", id);
        response.set_header("Cache-Control", "no-store");
        response.set_header("Accept-Ranges", "bytes");
        response.set_header("ETag", revision);
        response.set_header("Content-Disposition", axk::server::attachment_content_disposition(file->filename));
    };
    if (request.method == crow::HTTPMethod::HEAD) {
        if (const auto unchanged = file->verify_unchanged(); !unchanged)
            return error_response(409, unchanged.error(), id);
        response.code = 200;
        response.set_header("Content-Length", std::to_string(size));
        set_common_headers();
        audit(id, "file_download_inspect", "allowed", request_owner(request), "root", root_id);
        return response;
    }

    const auto range_header = request.get_header_value("Range");
    std::uint64_t offset{};
    auto length = size;
    if (range_header.empty()) {
        if (size > config_.maximum_download_range_bytes)
            return error_response(413, {"download_too_large", "file requires a bounded byte range"}, id);
        response.code = 200;
    } else {
        const auto expected_revision = request.get_header_value("If-Match");
        if (expected_revision.empty())
            return error_response(
                428, {"download_precondition_required", "ranged file downloads require an If-Match revision"}, id);
        if (expected_revision != revision) {
            response = error_response(412, {"download_revision_changed", "sandbox file revision changed"}, id);
            response.set_header("ETag", revision);
            return response;
        }
        const auto range = parse_byte_range(range_header, size, config_.maximum_download_range_bytes);
        if (!range) {
            response = error_response(416, {"invalid_range", "requested byte range is invalid or too large"}, id);
            response.set_header("Content-Range", "bytes */" + std::to_string(size));
            return response;
        }
        offset = range->offset;
        length = range->length;
        response.code = 206;
        response.set_header("Content-Range", "bytes " + std::to_string(range->offset) + "-" +
                                                 std::to_string(range->offset + range->length - 1U) + "/" +
                                                 std::to_string(size));
    }
    const auto bytes = axk::server::detail::read_verified_download(*file, offset, length);
    if (!bytes) {
        const auto status = bytes.error().code == "archive_source_changed" ? 409 : 422;
        return error_response(status, bytes.error(), id);
    }
    response.body.assign(reinterpret_cast<const char *>(bytes->data()), bytes->size());
    set_common_headers();
    audit(id, "file_download", "allowed", request_owner(request), "root", root_id);
    return response;
}

void ServerApplication::download_archive_response(const crow::request &request, crow::response &response,
                                                  const std::string &archive_id) {
    const auto finish = [&response](crow::response value) {
        response = std::move(value);
        response.end();
    };
    const auto id = request_id(request);
    if (auto denied = guard(request, id)) {
        finish(std::move(*denied));
        return;
    }
    const axk::app::DownloadArchiveRef reference{archive_id};
    if (request.method == crow::HTTPMethod::Delete) {
        const auto removed = download_archives_.remove(reference, request_owner(request));
        if (!removed) {
            audit(id, "archive_delete", "denied", request_owner(request), "archive", archive_id);
            finish(error_response(status_for_error(removed.error()), removed.error(), id));
            return;
        }
        audit(id, "archive_delete", "allowed", request_owner(request), "archive", archive_id);
        finish(crow::response{204});
        return;
    }
    auto budget = archive_download_budget_.try_acquire();
    if (!budget) {
        finish(error_response(429,
                              {"archive_download_capacity_exhausted",
                               "maximum concurrent archive downloads are already active",
                               {},
                               true},
                              id));
        return;
    }
    const auto content = download_archives_.open(reference, request_owner(request));
    if (!content) {
        finish(error_response(status_for_error(content.error()), content.error(), id));
        return;
    }
    response.set_static_file_info_unsafe(axk::text::path_to_utf8(content->path), content->snapshot.media_type);
    if (response.code != 200) {
        finish(error_response(500, {"archive_storage_unavailable", "download archive cannot be streamed"}, id));
        return;
    }
    response.set_header("X-Request-Id", id);
    response.set_header("Cache-Control", "no-store");
    response.set_header("Content-Disposition", axk::server::attachment_content_disposition(content->snapshot.filename));
    audit(id, "archive_download", "allowed", request_owner(request), "archive", archive_id);
    response.end();
}

} // namespace axk::server::detail
