#include "axklib/application/midi_operations.hpp"

#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/sequence.hpp"

namespace {

using Json = nlohmann::json;
constexpr std::uint64_t maximum_midi_bytes = 16U * 1024U * 1024U;

struct ResolvedMidi {
    std::shared_ptr<const axk::RandomAccessReader> reader;
    std::optional<axk::app::UploadLease> lease;
};

axk::app::Error operation_error(std::string code, std::string message) { return {std::move(code), std::move(message)}; }

axk::app::Result<ResolvedMidi> resolve_midi(const Json &input, std::string_view owner_id,
                                            const axk::app::Sandbox &sandbox, axk::app::UploadStore &uploads) {
    try {
        const auto &source = input.at("source");
        const auto has_file = source.contains("fileRef");
        const auto has_upload = source.contains("uploadRef");
        if (has_file == has_upload)
            return std::unexpected(
                operation_error("invalid_request", "source must contain exactly one of fileRef or uploadRef"));
        if (has_file) {
            const auto &reference = source.at("fileRef");
            auto opened = sandbox.open_file(
                {reference.at("rootId").get<std::string>(), reference.at("relativePath").get<std::string>()});
            if (!opened)
                return std::unexpected(opened.error());
            return ResolvedMidi{std::move(opened->reader), std::nullopt};
        }

        const axk::app::UploadRef reference{source.at("uploadRef").at("uploadId").get<std::string>()};
        auto snapshot = uploads.inspect(reference, owner_id);
        if (!snapshot)
            return std::unexpected(snapshot.error());
        if (snapshot->kind != axk::app::UploadKind::midi)
            return std::unexpected(operation_error("upload_kind_mismatch", "upload is not a Standard MIDI File"));
        auto lease = uploads.lease(reference, owner_id);
        if (!lease)
            return std::unexpected(lease.error());
        auto reader = axk::FileReader::open(lease->path());
        if (!reader)
            return std::unexpected(operation_error("midi_inspection_failed", reader.error().message));
        return ResolvedMidi{std::move(*reader), std::move(*lease)};
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "MIDI source reference is malformed"));
    }
}

axk::app::Result<Json> inspect_midi(const Json &input, const axk::app::OperationContext &context,
                                    const axk::app::Sandbox &sandbox, axk::app::UploadStore &uploads) {
    if (context.cancellation.is_cancelled())
        return std::unexpected(operation_error("operation_cancelled", "MIDI inspection was cancelled"));
    auto resolved = resolve_midi(input, context.owner_id, sandbox, uploads);
    if (!resolved)
        return std::unexpected(resolved.error());
    const auto byte_count = resolved->reader->size();
    if (byte_count == 0U || byte_count > maximum_midi_bytes || byte_count > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(
            operation_error("midi_inspection_failed", "MIDI file size is outside the supported 1..16 MiB range"));
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(byte_count));
    if (auto read = resolved->reader->read_exact_at(0U, bytes); !read)
        return std::unexpected(operation_error("midi_inspection_failed", read.error().message));
    if (context.cancellation.is_cancelled())
        return std::unexpected(operation_error("operation_cancelled", "MIDI inspection was cancelled"));
    auto inspected = axk::inspect_smf0(bytes);
    if (!inspected)
        return std::unexpected(operation_error("midi_inspection_failed", inspected.error().message));
    auto controllers = Json::array();
    for (const auto &controller : inspected->controllers)
        controllers.push_back({{"controller", controller.controller}, {"eventCount", controller.event_count}});
    return Json{{"format", inspected->format},
                {"trackCount", inspected->track_count},
                {"ticksPerQuarterNote", inspected->ticks_per_quarter_note},
                {"endTick", inspected->end_tick},
                {"eventCount", inspected->event_count},
                {"channelEventCount", inspected->channel_event_count},
                {"metaEventCount", inspected->meta_event_count},
                {"systemExclusiveEventCount", inspected->system_exclusive_event_count},
                {"systemExclusiveDataBytes", inspected->system_exclusive_data_bytes},
                {"controllers", std::move(controllers)},
                {"systemExclusiveManufacturerIds", inspected->system_exclusive_manufacturer_ids},
                {"systemExclusivePreservationSupported", inspected->system_exclusive_preservation_supported}};
}

} // namespace

axk::app::Result<void> axk::app::bind_midi_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                      UploadStore &uploads) {
    return registry.bind("midi.inspect", [&sandbox, &uploads](const auto &input, const auto &context) {
        return inspect_midi(input, context, sandbox, uploads);
    });
}
