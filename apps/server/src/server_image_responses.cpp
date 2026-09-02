#include "server_application.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "authentication.hpp"
#include "axklib/application/image_session_contracts.hpp"
#include "axklib/server/job_json.hpp"
#include "axklib/server/telemetry.hpp"
#include "axklib/utf8.hpp"
#include "axklib/writer.hpp"
#include "download_reader.hpp"
#include "http_headers.hpp"
#include "route_registration.hpp"
#include "server_support.hpp"

namespace axk::server::detail {
namespace {

std::string_view system_program_context_availability_name(axk::app::SystemProgramContextAvailability availability) {
    switch (availability) {
    case axk::app::SystemProgramContextAvailability::available:
        return "AVAILABLE";
    case axk::app::SystemProgramContextAvailability::not_present:
        return "NOT_PRESENT";
    case axk::app::SystemProgramContextAvailability::invalid:
        return "INVALID";
    }
    return "INVALID";
}

std::string_view system_program_context_file_name(axk::app::SystemProgramContextFile file) {
    return file == axk::app::SystemProgramContextFile::system ? "SYSTEM" : "SYSTEM2";
}

axk::app::ImageSourceRef parse_image_source(const Json &reference) {
    auto source = axk::app::image_source_ref_from_json(reference);
    if (!source)
        throw Json::type_error::create(302, source.error().message, &reference);
    return std::move(*source);
}

} // namespace

Json ServerApplication::image_summary_json(const axk::app::ImageSessionSummary &summary) const {
    return axk::app::image_session_summary_json(summary);
}

crow::response ServerApplication::attach_companions_response(const crow::request &request,
                                                             const std::string &image_id) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto parsed = parse_validated_json_body(request, "ImageCompanionsRequest");
    if (!parsed)
        return error_response(status_for_error(parsed.error(), 400), parsed.error(), id);

    std::uint64_t expected_revision{};
    axk::app::CompanionSelection selection;
    try {
        expected_revision = parsed->at("expectedRevision").get<std::uint64_t>();
        const auto &wire_selection = parsed->at("selection");
        const auto kind = wire_selection.at("kind").get<std::string>();
        if (kind == "SOURCES") {
            selection.kind = axk::app::CompanionSelectionKind::sources;
            for (const auto &source : wire_selection.at("sources"))
                selection.sources.push_back(parse_image_source(source));
        } else if (kind == "IMMEDIATE_SIBLINGS") {
            selection.kind = axk::app::CompanionSelectionKind::immediate_siblings;
        } else {
            return error_response(400, {"invalid_request", "companion source selection kind is unsupported"}, id);
        }
    } catch (const Json::exception &) {
        return error_response(
            400, {"invalid_request", "expectedRevision and a valid companion source selection are required"}, id);
    }

    const auto summary = images_.attach_companions(image_id, request_owner(request), expected_revision, selection);
    if (!summary)
        return error_response(status_for_error(summary.error()), summary.error(), id);
    return json_response(200, {{"data", image_summary_json(*summary)}, {"meta", {{"requestId", id}}}}, id);
}

crow::response ServerApplication::image_response(const crow::request &request, const std::string &image_id) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    if (request.method == crow::HTTPMethod::Delete) {
        const auto closed = images_.close(image_id, request_owner(request));
        if (!closed)
            return error_response(status_for_error(closed.error()), closed.error(), id);
        return json_response(200, {{"data", {{"closed", true}}}, {"meta", {{"requestId", id}}}}, id);
    }
    const auto summary = images_.inspect(image_id, request_owner(request));
    if (!summary)
        return error_response(status_for_error(summary.error()), summary.error(), id);
    return json_response(200, {{"data", image_summary_json(*summary)}, {"meta", {{"requestId", id}}}}, id);
}

template <typename Item, typename Loader, typename Serializer>
crow::response ServerApplication::image_page_response(const crow::request &request, const std::string &image_id,
                                                      Loader loader, Serializer serializer) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    std::size_t limit = std::min<std::size_t>(200U, config_.maximum_page_size);
    if (const auto *value = request.url_params.get("limit"); value != nullptr) {
        const auto parsed = parse_unsigned(value);
        if (!parsed || *parsed > std::numeric_limits<std::size_t>::max())
            return error_response(400, {"invalid_page", "limit must be an unsigned integer"}, id);
        limit = static_cast<std::size_t>(*parsed);
    }
    std::optional<std::string_view> cursor;
    if (const auto *value = request.url_params.get("cursor"); value != nullptr && *value != '\0') {
        cursor = value;
        if (cursor->size() > maximum_cursor_length)
            return error_response(400, {"invalid_cursor", "cursor length is outside the configured contract"}, id);
    }
    const auto page = loader(image_id, request_owner(request), limit, cursor);
    if (!page)
        return error_response(status_for_error(page.error()), page.error(), id);
    Json items = Json::array();
    for (const Item &item : page->items)
        items.push_back(serializer(item));
    return json_response(200,
                         {{"data",
                           {{"items", std::move(items)},
                            {"totalCount", page->total_count},
                            {"nextCursor", page->next_cursor ? Json(*page->next_cursor) : Json{}}}},
                          {"meta", {{"requestId", id}}}},
                         id);
}

crow::response ServerApplication::image_content_response(const crow::request &request, const std::string &image_id) {
    std::optional<std::string_view> parent_id;
    if (const auto *value = request.url_params.get("parentId"); value != nullptr && *value != '\0')
        parent_id = value;
    return image_page_response<axk::app::ImageContentItem>(
        request, image_id,
        [this, parent_id](auto id, auto owner, auto limit, auto cursor) {
            return images_.content(id, owner, limit, cursor, parent_id);
        },
        [](const axk::app::ImageContentItem &item) {
            return Json{
                {"id", item.id},
                {"parentId", item.parent_id ? Json(*item.parent_id) : Json{}},
                {"depth", item.depth},
                {"partitionIndex", item.partition_index ? Json(*item.partition_index) : Json{}},
                {"volumeDirectoryId", item.volume_directory_id ? Json(*item.volume_directory_id) : Json{}},
                {"partitionCapacity", item.partition_capacity
                                          ? Json{{"allocatedClusters", item.partition_capacity->allocated_clusters},
                                                 {"freeClusters", item.partition_capacity->free_clusters},
                                                 {"clusterSizeBytes", item.partition_capacity->cluster_size_bytes}}
                                          : Json{}},
                {"sizeBytes", item.size_bytes ? Json(*item.size_bytes) : Json{}},
                {"kind", item.kind},
                {"name", item.name},
                {"displayName", item.display_name},
                {"childCount", item.child_count},
                {"objectId", item.object_id ? Json(*item.object_id) : Json{}},
                {"objectType", item.object_type ? Json(*item.object_type) : Json{}},
                {"scopeRole", item.scope_role},
                {"quality", item.quality},
                {"basis", item.basis},
                {"notes", item.notes},
                {"details", item.details}};
        });
}

crow::response ServerApplication::image_objects_response(const crow::request &request, const std::string &image_id) {
    std::optional<std::string_view> object_type;
    if (const auto *value = request.url_params.get("type"); value != nullptr && *value != '\0')
        object_type = value;
    std::optional<std::string_view> content_scope_id;
    if (const auto *value = request.url_params.get("scopeId"); value != nullptr && *value != '\0')
        content_scope_id = value;
    return image_page_response<axk::app::ImageObjectItem>(
        request, image_id,
        [this, object_type, content_scope_id](auto id, auto owner, auto limit, auto cursor) {
            return images_.objects(id, owner, limit, cursor, object_type, content_scope_id);
        },
        [](const axk::app::ImageObjectItem &item) {
            Json waveform;
            if (item.waveform) {
                waveform = {{"sampleRate", item.waveform->sample_rate},
                            {"sampleWidthBytes", item.waveform->sample_width_bytes},
                            {"embeddedContainerName", item.waveform->embedded_container_name},
                            {"rootKey", item.waveform->root_key},
                            {"fineTuneCents", item.waveform->fine_tune_cents},
                            {"loopMode", item.waveform->loop_mode},
                            {"loopModeLabel", item.waveform->loop_mode_label},
                            {"storedFrameCount", item.waveform->stored_frame_count},
                            {"waveStartFrame", item.waveform->wave_start_frame},
                            {"waveLengthFrames", item.waveform->wave_length_frames},
                            {"loopStartFrame", item.waveform->loop_start_frame},
                            {"loopLengthFrames", item.waveform->loop_length_frames},
                            {"storageState", item.waveform->storage_state}};
            }
            Json sequence;
            if (item.sequence) {
                Json tempo_events = Json::array();
                for (const auto &event : item.sequence->tempo_events) {
                    tempo_events.push_back(
                        {{"tick", event.tick}, {"microsecondsPerQuarterNote", event.microseconds_per_quarter_note}});
                }
                sequence = {{"formatVersion", item.sequence->format_version},
                            {"ticksPerQuarterNote", item.sequence->ticks_per_quarter_note},
                            {"firstTick", item.sequence->first_tick},
                            {"endTick", item.sequence->end_tick},
                            {"eventCount", item.sequence->event_count},
                            {"headerTempoBpm",
                             item.sequence->header_tempo_bpm ? Json(*item.sequence->header_tempo_bpm) : Json{}},
                            {"effectiveInitialTempoMicrosecondsPerQuarterNote",
                             item.sequence->effective_initial_tempo_microseconds_per_quarter_note},
                            {"tempoEvents", std::move(tempo_events)}};
            }
            return Json{{"id", item.id},
                        {"type", item.type},
                        {"name", item.name},
                        {"format", item.format},
                        {"partitionIndex", item.partition_index ? Json(*item.partition_index) : Json{}},
                        {"partitionName", item.partition_name},
                        {"volumeName", item.volume_name},
                        {"categoryName", item.category_name},
                        {"entryName", item.entry_name},
                        {"sizeBytes", item.stored_size_bytes},
                        {"sizeWithDependenciesBytes",
                         item.size_with_dependencies_bytes ? Json(*item.size_with_dependencies_bytes) : Json{}},
                        {"waveform", std::move(waveform)},
                        {"sequence", std::move(sequence)}};
        });
}

crow::response ServerApplication::image_object_response(const crow::request &request, const std::string &image_id,
                                                        const std::string &object_id) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto metadata = images_.object_detail(image_id, request_owner(request), object_id);
    if (!metadata)
        return error_response(status_for_error(metadata.error()), metadata.error(), id);
    return json_response(200, {{"data", *metadata}, {"meta", {{"requestId", id}}}}, id);
}

crow::response ServerApplication::image_relationships_response(const crow::request &request,
                                                               const std::string &image_id) {
    axk::app::ImageRelationshipFilter filter;
    if (const auto *value = request.url_params.get("scopeId"); value != nullptr && *value != '\0')
        filter.content_scope_id = value;
    if (const auto *value = request.url_params.get("sourceObjectId"); value != nullptr && *value != '\0')
        filter.source_object_id = value;
    if (const auto *value = request.url_params.get("targetObjectId"); value != nullptr && *value != '\0')
        filter.target_object_id = value;
    if (const auto *value = request.url_params.get("type"); value != nullptr && *value != '\0')
        filter.relationship_type = value;
    return image_page_response<axk::app::ImageRelationshipItem>(
        request, image_id,
        [this, filter](auto id, auto owner, auto limit, auto cursor) {
            return images_.relationships(id, owner, limit, cursor, filter);
        },
        [](const axk::app::ImageRelationshipItem &item) {
            return Json{{"id", item.id},
                        {"sourceObjectId", item.source_object_id},
                        {"targetObjectId", item.target_object_id ? Json(*item.target_object_id) : Json{}},
                        {"candidateObjectIds", item.candidate_object_ids},
                        {"type", item.type},
                        {"quality", item.quality},
                        {"basis", item.basis},
                        {"notes", item.notes},
                        {"assignmentIndex", item.assignment_index ? Json(*item.assignment_index) : Json{}},
                        {"assignmentName", item.assignment_name},
                        {"assignmentState", item.assignment_state},
                        {"receiveChannelDisplay", item.receive_channel_display}};
        });
}

crow::response ServerApplication::image_system_program_contexts_response(const crow::request &request,
                                                                         const std::string &image_id) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto *partition_text = request.url_params.get("partitionIndex");
    const auto partition = parse_unsigned(partition_text == nullptr ? "" : partition_text);
    if (!partition || *partition > 7U) {
        return error_response(400, {"invalid_partition", "partitionIndex must be an integer from 0 through 7"}, id);
    }
    const auto contexts =
        images_.system_program_contexts(image_id, request_owner(request), static_cast<std::uint8_t>(*partition));
    if (!contexts)
        return error_response(status_for_error(contexts.error()), contexts.error(), id);

    Json files = Json::array();
    for (const auto &context : contexts->files) {
        Json file{{"fileKind", system_program_context_file_name(context.file_kind)},
                  {"availability", system_program_context_availability_name(context.availability)}};
        if (context.availability == axk::app::SystemProgramContextAvailability::available) {
            file["model"] = context.model;
            file["basicReceive"] = {{"port", context.basic_receive->port},
                                    {"channel", context.basic_receive->channel},
                                    {"display", context.basic_receive->display}};
            file["omni"] = *context.omni;
            file["programChangeEnabled"] = *context.program_change_enabled;
            if (context.file_kind == axk::app::SystemProgramContextFile::system2) {
                Json parts = Json::array();
                for (const auto &part : context.parts) {
                    parts.push_back(
                        {{"partNumber", part.part_number},
                         {"partLabel", part.part_label},
                         {"midi",
                          {{"port", part.midi.port}, {"channel", part.midi.channel}, {"display", part.midi.display}}},
                         {"programNumber", part.program_number},
                         {"master", part.master}});
                }
                file["savedProgramMode"] = *context.saved_program_mode;
                file["parts"] = std::move(parts);
            }
        } else {
            file["message"] = context.message;
        }
        files.push_back(std::move(file));
    }
    Json data{
        {"partitionIndex", contexts->partition_index}, {"files", std::move(files)}, {"message", contexts->message}};
    return json_response(200, {{"data", std::move(data)}, {"meta", {{"requestId", id}}}}, id);
}

crow::response ServerApplication::image_validation_response(const crow::request &request, const std::string &image_id) {
    return image_page_response<axk::app::ImageValidationItem>(
        request, image_id,
        [this](auto id, auto owner, auto limit, auto cursor) {
            return images_.validation_issues(id, owner, limit, cursor);
        },
        [](const axk::app::ImageValidationItem &item) {
            return Json{{"code", item.code},
                        {"severity", item.severity},
                        {"message", item.message},
                        {"samplerPath", item.sampler_path},
                        {"objectId", item.object_id ? Json(*item.object_id) : Json{}}};
        });
}

crow::response ServerApplication::image_preview_response(const crow::request &request, const std::string &image_id) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto *object_id = request.url_params.get("objectId");
    const auto bins = parse_unsigned(request.url_params.get("bins") == nullptr ? "" : request.url_params.get("bins"));
    if (object_id == nullptr || *object_id == '\0' || !bins || *bins > std::numeric_limits<std::size_t>::max())
        return error_response(400, {"invalid_preview", "objectId and an unsigned bins value are required"}, id);
    const auto preview = images_.preview(image_id, request_owner(request), object_id, static_cast<std::size_t>(*bins));
    if (!preview)
        return error_response(status_for_error(preview.error()), preview.error(), id);
    Json lanes = Json::array();
    for (const auto &lane : preview->lanes) {
        Json values = Json::array();
        for (const auto &bin : lane.bins)
            values.push_back({{"minimum", bin.minimum}, {"maximum", bin.maximum}});
        lanes.push_back({{"role", lane.role},
                         {"sourceObjectId", lane.source_object_id},
                         {"frameCount", lane.frame_count},
                         {"bins", std::move(values)}});
    }
    return json_response(
        200,
        {{"data",
          {{"objectId", preview->object_id}, {"frameCount", preview->frame_count}, {"lanes", std::move(lanes)}}},
         {"meta", {{"requestId", id}}}},
        id);
}

crow::response ServerApplication::audition_content_response(const crow::request &request,
                                                            const std::string &audition_id) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto metadata = images_.audition_range(audition_id, request_owner(request), 0U, 0U);
    if (!metadata)
        return error_response(status_for_error(metadata.error()), metadata.error(), id);
    const auto total = metadata->total_size;
    crow::response response;
    const auto range_header = request.get_header_value("Range");
    std::optional<ByteRange> range;
    if (range_header.empty()) {
        if (total > config_.maximum_download_range_bytes) {
            response = error_response(
                416, {"range_required", "large audition content must be requested with a bounded byte range"}, id);
            response.set_header("Content-Range", "bytes */" + std::to_string(total));
            return response;
        }
        range = ByteRange{0U, total};
    } else {
        range = parse_byte_range(range_header, total, config_.maximum_download_range_bytes);
    }
    if (!range) {
        response = error_response(416, {"invalid_range", "requested audio range is invalid or too large"}, id);
        response.set_header("Content-Range", "bytes */" + std::to_string(total));
        return response;
    }
    auto bytes = images_.audition_range(audition_id, request_owner(request), range->offset,
                                        static_cast<std::size_t>(range->length));
    if (!bytes)
        return error_response(status_for_error(bytes.error()), bytes.error(), id);
    response.code = range_header.empty() ? 200 : 206;
    response.body.assign(reinterpret_cast<const char *>(bytes->bytes.data()), bytes->bytes.size());
    if (!range_header.empty()) {
        response.set_header("Content-Range", "bytes " + std::to_string(range->offset) + "-" +
                                                 std::to_string(range->offset + range->length - 1U) + "/" +
                                                 std::to_string(total));
    }
    response.set_header("Content-Type", "application/octet-stream");
    response.set_header("Accept-Ranges", "bytes");
    response.set_header("Cache-Control", "no-store");
    response.set_header("X-Request-Id", id);
    return response;
}

crow::response ServerApplication::audition_delete_response(const crow::request &request,
                                                           const std::string &audition_id) {
    const auto id = request_id(request);
    if (auto denied = guard(request, id))
        return std::move(*denied);
    const auto deleted = images_.delete_audition(audition_id, request_owner(request));
    if (!deleted)
        return error_response(status_for_error(deleted.error()), deleted.error(), id);
    crow::response response{204};
    response.set_header("X-Request-Id", id);
    return response;
}

} // namespace axk::server::detail
