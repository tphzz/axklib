#include "axklib/application/session_sequence_operations.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/application/download_archives.hpp"
#include "axklib/application/filesystem.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/sequence.hpp"

namespace {

using Json = nlohmann::json;

class TemporaryDirectoryCleanup {
  public:
    explicit TemporaryDirectoryCleanup(std::filesystem::path path) : path_{std::move(path)} {}
    ~TemporaryDirectoryCleanup() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

  private:
    std::filesystem::path path_;
};

axk::app::Error operation_error(std::string code, std::string message) { return {std::move(code), std::move(message)}; }

axk::app::Error core_error(const axk::Error &error) {
    axk::app::ErrorContext context;
    context.partition_index = error.context.partition_index;
    context.volume_name = error.context.volume_name;
    context.object_type = error.context.object_type;
    context.object_name = error.context.object_name;
    return {"sequence_export_failed", error.message, std::move(context)};
}

axk::app::Result<std::pair<std::string, std::uint64_t>> parse_session_identity(const Json &input) {
    try {
        const auto image_id = input.at("imageId").get<std::string>();
        const auto revision = input.at("expectedRevision").get<std::uint64_t>();
        if (image_id.empty() || revision == 0U)
            return std::unexpected(operation_error("invalid_request", "imageId and expectedRevision are required"));
        return std::pair{image_id, revision};
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "imageId and expectedRevision are required"));
    }
}

std::string safe_filename_stem(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) != 0 || character == ' ' || character == '-' || character == '_' || character == '.')
            result.push_back(character);
        else
            result.push_back('_');
    }
    while (!result.empty() && (result.front() == ' ' || result.front() == '.'))
        result.erase(result.begin());
    while (!result.empty() && (result.back() == ' ' || result.back() == '.'))
        result.pop_back();
    return result.empty() ? "Sequence" : result;
}

std::filesystem::path unique_midi_path(const std::filesystem::path &directory, std::string_view object_name,
                                       std::set<std::string, std::less<>> &used_names) {
    const auto stem = safe_filename_stem(object_name);
    auto filename = stem + ".mid";
    for (std::size_t suffix = 2U; !used_names.emplace(filename).second; ++suffix)
        filename = std::format("{} ({}){}", stem, suffix, ".mid");
    return directory / filename;
}

axk::app::Result<void> write_bytes(const std::filesystem::path &path, std::span<const std::byte> bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output)
        return std::unexpected(operation_error("sequence_export_failed", "could not create the MIDI file"));
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output)
        return std::unexpected(operation_error("sequence_export_failed", "could not write the MIDI file"));
    output.flush();
    if (!output)
        return std::unexpected(operation_error("sequence_export_failed", "could not flush the MIDI file"));
    return {};
}

axk::app::Result<std::vector<const axk::ObjectSnapshot *>>
resolve_sequences(const Json &input, const axk::app::ImageSessionRead &session) {
    try {
        const auto &object_ids = input.at("objectIds");
        if (!object_ids.is_array() || object_ids.empty() || object_ids.size() > 1024U)
            return std::unexpected(operation_error("invalid_request", "objectIds must contain 1 to 1024 entries"));
        std::unordered_map<std::string_view, const axk::ObjectSnapshot *> objects_by_key;
        objects_by_key.reserve(session.catalog_objects.size());
        for (const auto *object : session.catalog_objects)
            objects_by_key.emplace(object->key, object);
        std::set<std::string, std::less<>> unique_ids;
        std::vector<const axk::ObjectSnapshot *> result;
        result.reserve(object_ids.size());
        for (const auto &value : object_ids) {
            const auto object_id = value.get<std::string>();
            if (object_id.empty() || !unique_ids.emplace(object_id).second)
                return std::unexpected(operation_error("invalid_request", "sequence object IDs must be unique"));
            const auto key = session.object_keys_by_id.find(object_id);
            if (key == session.object_keys_by_id.end())
                return std::unexpected(operation_error("object_not_found", "sequence object does not exist"));
            const auto object = objects_by_key.find(key->second);
            if (object == objects_by_key.end() || object->second->object.header.raw_type != "SEQU")
                return std::unexpected(operation_error("invalid_request", "every selected object must be a Sequence"));
            if (!std::holds_alternative<axk::CurrentSequence>(object->second->object.payload))
                return std::unexpected(operation_error("sequence_export_failed", "Sequence payload is not decoded"));
            result.push_back(object->second);
        }
        return result;
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "sequence export request is malformed"));
    }
}

axk::app::Result<axk::app::DirectoryRef> workspace_destination(const Json &destination) {
    try {
        const auto &output = destination.at("output");
        const auto root_id = output.at("rootId").get<std::string>();
        const auto relative_path = output.at("relativePath").get<std::string>();
        if (root_id.empty() || relative_path.empty())
            return std::unexpected(
                operation_error("invalid_request", "workspace destination requires an output directory"));
        return axk::app::DirectoryRef{root_id, relative_path};
    } catch (const Json::exception &) {
        return std::unexpected(
            operation_error("invalid_request", "workspace destination requires an output directory"));
    }
}

void report_progress(const axk::app::OperationContext &context, axk::ProgressPhase phase, std::size_t completed,
                     std::size_t total, std::string label) {
    if (context.progress)
        context.progress->report({phase, completed, total, std::move(label), std::nullopt});
}

} // namespace

axk::app::Result<void> axk::app::bind_session_sequence_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                                  ImageSessionManager &images,
                                                                  DownloadArchiveStore &downloads) {
    if (registry.is_implemented("images.sequence_export"))
        return {};
    return registry.bind(
        "images.sequence_export",
        [&sandbox, &images, &downloads](const Json &input, const OperationContext &context) -> Result<Json> {
            const auto identity = parse_session_identity(input);
            if (!identity)
                return std::unexpected(identity.error());
            auto session = images.begin_read(identity->first, context.owner_id, identity->second);
            if (!session)
                return std::unexpected(session.error());
            const auto sequences = resolve_sequences(input, *session);
            if (!sequences)
                return std::unexpected(sequences.error());
            report_progress(context, axk::ProgressPhase::resolving, 0U, sequences->size() + 1U,
                            "Resolving Sequence selection");
            auto staging = sandbox.create_staging_directory("axklib-sequence-export");
            if (!staging)
                return std::unexpected(staging.error());
            TemporaryDirectoryCleanup cleanup{*staging};
            std::set<std::string, std::less<>> used_names;
            for (std::size_t index = 0U; index < sequences->size(); ++index) {
                if (context.cancellation.is_cancelled())
                    return std::unexpected(operation_error("cancelled", "Sequence export was cancelled"));
                const auto &object = *(*sequences)[index];
                const auto &sequence = std::get<axk::CurrentSequence>(object.object.payload);
                auto midi = axk::sequence_to_smf0(sequence);
                if (!midi)
                    return std::unexpected(core_error(midi.error()));
                const auto path = unique_midi_path(*staging, object.object.header.name, used_names);
                if (auto written = write_bytes(path, *midi); !written)
                    return std::unexpected(written.error());
                report_progress(context, axk::ProgressPhase::writing, index + 1U, sequences->size() + 1U,
                                "Writing MIDI files");
            }
            session->lease.reset();

            Json destination;
            try {
                destination = input.at("destination");
            } catch (const Json::exception &) {
                return std::unexpected(operation_error("invalid_request", "destination is required"));
            }
            Json result{{"imageId", identity->first},
                        {"revision", identity->second},
                        {"sequenceCount", sequences->size()},
                        {"fileCount", sequences->size()}};
            const auto kind = destination.value("kind", std::string{});
            report_progress(context, axk::ProgressPhase::publishing, sequences->size(), sequences->size() + 1U,
                            "Publishing MIDI export");
            if (kind == "WORKSPACE") {
                const auto output = workspace_destination(destination);
                if (!output)
                    return std::unexpected(output.error());
                if (auto published = sandbox.publish_directory(*output, false, *staging); !published)
                    return std::unexpected(published.error());
                result["destination"] = "WORKSPACE";
                result["output"] = {{"rootId", output->root_id}, {"relativePath", output->relative_path}};
                result["download"] = nullptr;
            } else if (kind == "DOWNLOAD") {
                const auto directory_name = destination.value("directoryName", std::string{});
                if (directory_name.empty() || directory_name == "." || directory_name == ".." ||
                    directory_name.find_first_of("/\\") != std::string::npos) {
                    return std::unexpected(operation_error("invalid_request", "download directory name is invalid"));
                }
                auto retained = downloads.create_owned_directory(context.owner_id, *staging, directory_name + ".tar");
                if (!retained)
                    return std::unexpected(retained.error());
                result["destination"] = "DOWNLOAD";
                result["output"] = nullptr;
                result["download"] = {
                    {"archiveId", retained->reference.archive_id},
                    {"filename", retained->filename},
                    {"sizeBytes", retained->size_bytes},
                    {"expiresInSeconds", retained->expires_in_seconds},
                    {"contentPath", "/api/v1/download-archives/" + retained->reference.archive_id + "/content"}};
            } else {
                return std::unexpected(
                    operation_error("invalid_request", "destination kind must be WORKSPACE or DOWNLOAD"));
            }
            report_progress(context, axk::ProgressPhase::publishing, sequences->size() + 1U, sequences->size() + 1U,
                            "MIDI export ready");
            return result;
        });
}
