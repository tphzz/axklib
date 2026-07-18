#include "axklib/application/audio_operations.hpp"

#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "axklib/writer.hpp"

namespace {

using Json = nlohmann::json;

struct ResolvedAudio {
    std::filesystem::path path;
    std::optional<axk::app::UploadLease> lease;
};

axk::app::Error operation_error(std::string code, std::string message) { return {std::move(code), std::move(message)}; }

axk::app::Error core_error(const axk::Error &error) {
    return {error.code == axk::ErrorCode::operation_cancelled ? "operation_cancelled" : "audio_inspection_failed",
            error.message};
}

axk::app::Result<ResolvedAudio> resolve_audio(const Json &input, std::string_view owner_id,
                                              const axk::app::Sandbox &sandbox, axk::app::UploadStore &uploads) {
    try {
        const auto &source = input.at("source");
        const auto has_file = source.contains("fileRef");
        const auto has_upload = source.contains("uploadRef");
        if (has_file == has_upload) {
            return std::unexpected(
                operation_error("invalid_request", "source must contain exactly one of fileRef or uploadRef"));
        }
        if (has_file) {
            const auto &reference = source.at("fileRef");
            auto path = sandbox.resolve_file(
                {reference.at("rootId").get<std::string>(), reference.at("relativePath").get<std::string>()});
            if (!path)
                return std::unexpected(path.error());
            return ResolvedAudio{std::move(*path), std::nullopt};
        }

        const axk::app::UploadRef reference{source.at("uploadRef").at("uploadId").get<std::string>()};
        auto snapshot = uploads.inspect(reference, owner_id);
        if (!snapshot)
            return std::unexpected(snapshot.error());
        if (snapshot->kind != axk::app::UploadKind::audio)
            return std::unexpected(operation_error("upload_kind_mismatch", "upload is not sampler audio"));
        auto lease = uploads.lease(reference, owner_id);
        if (!lease)
            return std::unexpected(lease.error());
        auto path = lease->path();
        return ResolvedAudio{std::move(path), std::move(*lease)};
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "audio source reference is malformed"));
    }
}

axk::app::Result<Json> inspect_audio(const Json &input, const axk::app::OperationContext &context,
                                     const axk::app::Sandbox &sandbox, axk::app::UploadStore &uploads) {
    if (context.cancellation.is_cancelled())
        return std::unexpected(operation_error("operation_cancelled", "audio inspection was cancelled"));
    auto resolved = resolve_audio(input, context.owner_id, sandbox, uploads);
    if (!resolved)
        return std::unexpected(resolved.error());
    auto inspected = axk::inspect_sampler_audio(resolved->path);
    if (!inspected)
        return std::unexpected(core_error(inspected.error()));
    const auto duration = inspected->source_sample_rate == 0U ? 0.0
                                                              : static_cast<double>(inspected->frame_count) /
                                                                    static_cast<double>(inspected->source_sample_rate);
    return Json{{"sourceFormat", inspected->source_format},
                {"sourceSubtype", inspected->source_subtype},
                {"channels", inspected->channels},
                {"frameCount", inspected->frame_count},
                {"sourceSampleRate", inspected->source_sample_rate},
                {"outputSampleRate", inspected->output_sample_rate},
                {"durationSeconds", duration},
                {"resampled", inspected->resampled},
                {"quantized", inspected->quantized}};
}

} // namespace

axk::app::Result<void> axk::app::bind_audio_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                       UploadStore &uploads) {
    return registry.bind("audio.inspect", [&sandbox, &uploads](const auto &input, const auto &context) {
        return inspect_audio(input, context, sandbox, uploads);
    });
}
