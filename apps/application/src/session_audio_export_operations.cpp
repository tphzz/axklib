#include "axklib/application/session_audio_export_operations.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/application/download_archives.hpp"
#include "axklib/application/extraction_selection.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/audio_export.hpp"
#include "axklib/relationship.hpp"
#include "axklib/utf8.hpp"
#include "relationship_diagnostic.hpp"

namespace {

using Json = nlohmann::json;

struct AudioExportRoot {
    std::string kind;
    std::optional<std::uint8_t> partition_index;
    std::string volume_name;
    std::string object_key;
};

struct AudioExportSelection {
    std::set<std::pair<std::uint8_t, std::string>> volumes;
    axk::app::ExactExportClosure closure;
    std::vector<Json> issues;
    std::string default_directory_name;
    std::size_t sfz_file_count{};

    [[nodiscard]] bool sfz_eligible() const noexcept { return sfz_file_count != 0U && issues.empty(); }
};

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

axk::app::Error operation_error(std::string code, std::string message,
                                std::optional<std::string> relative_path = std::nullopt) {
    axk::app::ErrorContext context;
    context.relative_path = std::move(relative_path);
    return {std::move(code), std::move(message), std::move(context)};
}

axk::app::Error core_error(const axk::Error &error, std::optional<std::string> relative_path = std::nullopt) {
    axk::app::ErrorContext context;
    context.partition_index = error.context.partition_index;
    context.volume_name = error.context.volume_name;
    context.object_type = error.context.object_type;
    context.object_name = error.context.object_name;
    context.relative_path = std::move(relative_path);
    return {"audio_export_failed", error.message, std::move(context)};
}

std::string safe_directory_name(std::string value) {
    std::string result;
    bool prior_space{};
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isspace(byte) != 0) {
            if (!result.empty() && !prior_space)
                result.push_back(' ');
            prior_space = true;
            continue;
        }
        prior_space = false;
        if (std::isalnum(byte) != 0 || character == '-' || character == '_' || character == '.')
            result.push_back(character);
        else
            result.push_back('_');
    }
    while (!result.empty() && (result.front() == ' ' || result.front() == '.'))
        result.erase(result.begin());
    while (!result.empty() && (result.back() == ' ' || result.back() == '.'))
        result.pop_back();
    return result.empty() ? "Audio export" : result;
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

axk::app::Result<std::vector<AudioExportRoot>>
parse_roots(const Json &input, const std::unordered_map<std::string, std::string> &object_keys_by_id) {
    try {
        const auto &values = input.at("roots");
        if (!values.is_array() || values.empty() || values.size() > 1024U)
            return std::unexpected(operation_error("invalid_request", "roots must contain 1 to 1024 selectors"));
        std::vector<AudioExportRoot> roots;
        std::set<std::string, std::less<>> identities;
        roots.reserve(values.size());
        for (const auto &value : values) {
            AudioExportRoot root;
            root.kind = value.at("kind").get<std::string>();
            std::string identity;
            if (root.kind == "VOLUME") {
                const auto partition = value.at("partitionIndex").get<std::uint32_t>();
                if (partition > 255U)
                    return std::unexpected(operation_error("invalid_request", "partitionIndex is out of range"));
                root.partition_index = static_cast<std::uint8_t>(partition);
                root.volume_name = value.at("volumeName").get<std::string>();
                if (root.volume_name.empty() || value.size() != 3U) {
                    return std::unexpected(
                        operation_error("invalid_request", "volume roots require partitionIndex and volumeName"));
                }
                identity = std::format("VOLUME\\0{}\\0{}", partition, root.volume_name);
            } else {
                if (root.kind != "PROGRAM" && root.kind != "SBAC" && root.kind != "SBNK" && root.kind != "SMPL")
                    return std::unexpected(operation_error("invalid_request", "audio export root kind is unsupported"));
                const auto object_id = value.at("objectId").get<std::string>();
                if (object_id.empty() || value.size() != 2U)
                    return std::unexpected(
                        operation_error("invalid_request", "object roots require exactly kind and objectId"));
                const auto found = object_keys_by_id.find(object_id);
                if (found == object_keys_by_id.end())
                    return std::unexpected(operation_error("object_not_found", "audio export root does not exist"));
                root.object_key = found->second;
                identity = "OBJECT\\0" + object_id;
            }
            if (!identities.emplace(std::move(identity)).second)
                return std::unexpected(operation_error("invalid_request", "audio export roots must be unique"));
            roots.push_back(std::move(root));
        }
        return roots;
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "audio export roots are malformed"));
    }
}

std::optional<std::string_view> expected_object_type(std::string_view kind) {
    if (kind == "PROGRAM")
        return "PROG";
    if (kind == "SBAC")
        return "SBAC";
    if (kind == "SBNK")
        return "SBNK";
    if (kind == "SMPL")
        return "SMPL";
    return std::nullopt;
}

void insert_object(AudioExportSelection &selection, const axk::ObjectSnapshot &object) {
    const auto type = object.object.header.raw_type;
    if (type == "PROG")
        selection.closure.programs.insert(object.key);
    else if (type == "SBAC")
        selection.closure.sample_banks.insert(object.key);
    else if (type == "SBNK")
        selection.closure.samples.insert(object.key);
    else if (type == "SMPL")
        selection.closure.wave_data.insert(object.key);
}

axk::app::Result<AudioExportSelection> resolve_selection(const std::vector<AudioExportRoot> &roots,
                                                         const axk::ObjectCatalog &catalog,
                                                         const axk::RelationshipGraph &graph) {
    std::unordered_map<std::string, const axk::ObjectSnapshot *> by_key;
    by_key.reserve(catalog.objects.size());
    for (const auto &object : catalog.objects)
        by_key.emplace(object.key, &object);

    AudioExportSelection selection;
    std::vector<std::string> root_names;
    root_names.reserve(roots.size());
    for (const auto &root : roots) {
        if (root.kind == "VOLUME") {
            const auto volume = std::pair{*root.partition_index, root.volume_name};
            bool found{};
            for (const auto &object : catalog.objects) {
                if (!object.placement || object.partition.value != volume.first ||
                    object.placement->volume_name != volume.second) {
                    continue;
                }
                found = true;
                insert_object(selection, object);
            }
            if (!found)
                return std::unexpected(operation_error("object_not_found", "audio export volume does not exist"));
            selection.volumes.insert(volume);
            root_names.push_back(root.volume_name);
            continue;
        }
        const auto object = by_key.find(root.object_key);
        if (object == by_key.end())
            return std::unexpected(operation_error("object_not_found", "audio export root does not exist"));
        const auto expected = expected_object_type(root.kind);
        if (!expected || object->second->object.header.raw_type != *expected)
            return std::unexpected(operation_error("invalid_request", "audio export root kind does not match object"));
        insert_object(selection, *object->second);
        root_names.push_back(object->second->object.header.name);
    }

    selection.closure = axk::app::build_exact_export_closure(
        graph, std::move(selection.closure.programs), std::move(selection.closure.sample_banks),
        std::move(selection.closure.samples), std::move(selection.closure.wave_data));
    for (const auto &relationship : selection.closure.excluded)
        selection.issues.push_back(
            axk::app::relationship_diagnostic(relationship, "Unconfirmed relationship excluded from exact export"));
    const auto sample_has_wave_data = [&](std::string_view sample_key) {
        return std::ranges::any_of(selection.closure.sample_wave_data,
                                   [&](const auto &relationship) { return relationship.first == sample_key; });
    };
    for (const auto &bank : selection.closure.sample_banks) {
        const auto has_playable_member =
            std::ranges::any_of(selection.closure.sample_bank_members, [&](const auto &relationship) {
                return relationship.first == bank && sample_has_wave_data(relationship.second);
            });
        if (has_playable_member)
            ++selection.sfz_file_count;
    }
    for (const auto &sample : selection.closure.samples) {
        const auto is_bank_member = std::ranges::any_of(selection.closure.sample_bank_members,
                                                        [&](const auto &member) { return member.second == sample; });
        if (!is_bank_member && sample_has_wave_data(sample))
            ++selection.sfz_file_count;
    }
    if (selection.sfz_file_count == 0U) {
        selection.issues.push_back(
            {{"code", "sfz_semantics_unavailable"},
             {"message", "The selection has no confirmed Sample-to-Wave Data relationships; export it as WAV."},
             {"fatal", true}});
    }
    for (const auto &root : roots) {
        if (root.kind != "SMPL")
            continue;
        const auto referenced = std::ranges::any_of(graph.relationships, [&](const auto &relationship) {
            return relationship.target_key && *relationship.target_key == root.object_key &&
                   selection.closure.samples.contains(relationship.source_key) &&
                   (relationship.type == "SBNK_LEFT_MEMBER_TO_SMPL" ||
                    relationship.type == "SBNK_RIGHT_MEMBER_TO_SMPL") &&
                   relationship.quality == axk::RelationshipQuality::known;
        });
        if (!referenced) {
            selection.issues.push_back(
                {{"code", "wave_data_has_no_confirmed_sample"},
                 {"message", "Selected Wave Data has no confirmed referencing Sample; export it as WAV."},
                 {"fatal", true}});
        }
    }
    selection.default_directory_name =
        safe_directory_name(roots.size() == 1U ? root_names.front() : root_names.front() + " and others");
    return selection;
}

Json inspection_json(std::string_view image_id, std::uint64_t revision, std::size_t root_count,
                     const AudioExportSelection &selection) {
    return {{"imageId", image_id},
            {"revision", revision},
            {"rootCount", root_count},
            {"programCount", selection.closure.programs.size()},
            {"sampleBankCount", selection.closure.sample_banks.size()},
            {"sampleCount", selection.closure.samples.size()},
            {"waveDataCount", selection.closure.wave_data.size()},
            {"sfzFileCount", selection.sfz_file_count},
            {"sfzEligible", selection.sfz_eligible()},
            {"defaultDirectoryName", selection.default_directory_name},
            {"issues", selection.issues}};
}

axk::ObjectCatalog catalog_from_session(const axk::app::ImageSessionRead &session) {
    axk::ObjectCatalog catalog;
    catalog.objects.reserve(session.catalog_objects.size());
    for (const auto *object : session.catalog_objects)
        catalog.objects.push_back(*object);
    catalog.issues = session.catalog_issues;
    return catalog;
}

void report_progress(const axk::app::OperationContext &context, axk::ProgressPhase phase, std::size_t completed,
                     std::size_t total, std::string label) {
    if (context.progress)
        context.progress->report({phase, completed, total, std::move(label), std::nullopt});
}

} // namespace

axk::app::Result<void> axk::app::bind_session_audio_export_operations(OperationRegistry &registry,
                                                                      const Sandbox &sandbox,
                                                                      ImageSessionManager &images,
                                                                      DownloadArchiveStore &downloads) {
    if (!registry.is_implemented("images.audio_export.inspect")) {
        auto bound =
            registry.bind("images.audio_export.inspect",
                          [&images](const Json &input, const OperationContext &context) -> Result<Json> {
                              const auto identity = parse_session_identity(input);
                              if (!identity)
                                  return std::unexpected(identity.error());
                              auto session = images.begin_read(identity->first, context.owner_id, identity->second);
                              if (!session)
                                  return std::unexpected(session.error());
                              const auto roots = parse_roots(input, session->object_keys_by_id);
                              if (!roots)
                                  return std::unexpected(roots.error());
                              const auto catalog = catalog_from_session(*session);
                              const auto graph = axk::build_relationship_graph(catalog);
                              const auto selection = resolve_selection(*roots, catalog, graph);
                              if (!selection)
                                  return std::unexpected(selection.error());
                              return inspection_json(identity->first, identity->second, roots->size(), *selection);
                          });
        if (!bound)
            return bound;
    }

    if (!registry.is_implemented("images.audio_export")) {
        auto bound = registry.bind(
            "images.audio_export",
            [&sandbox, &images, &downloads](const Json &input, const OperationContext &context) -> Result<Json> {
                const auto identity = parse_session_identity(input);
                if (!identity)
                    return std::unexpected(identity.error());
                auto session = images.begin_read(identity->first, context.owner_id, identity->second);
                if (!session)
                    return std::unexpected(session.error());
                const auto roots = parse_roots(input, session->object_keys_by_id);
                if (!roots)
                    return std::unexpected(roots.error());
                const auto catalog = catalog_from_session(*session);
                const auto graph = axk::build_relationship_graph(catalog);
                const auto selection = resolve_selection(*roots, catalog, graph);
                if (!selection)
                    return std::unexpected(selection.error());

                const auto format = input.value("format", std::string{});
                if (format != "SFZ" && format != "WAV")
                    return std::unexpected(operation_error("invalid_request", "format must be SFZ or WAV"));
                if (format == "SFZ" && !selection->sfz_eligible()) {
                    return std::unexpected(operation_error(
                        "sfz_semantics_unavailable",
                        "The selection cannot produce reliable SFZ instruments; export it as WAV instead."));
                }
                if (selection->closure.wave_data.empty())
                    return std::unexpected(
                        operation_error("audio_export_empty", "The selection contains no exportable Wave Data."));

                report_progress(context, axk::ProgressPhase::resolving, 0U, 4U, "Resolving export selection");
                auto plan = axk::build_export_plan(*session->media, catalog, graph, context.cancellation);
                if (!plan && plan.error().code == axk::ErrorCode::object_missing &&
                    session->source.kind == ImageSourceKind::axk_object_directory) {
                    return std::unexpected(operation_error(
                        "companion_disks_required",
                        "Wave Data continues on another sampler disk. Add companion disk folders to export it.",
                        session->source.relative_path));
                }
                if (!plan)
                    return std::unexpected(core_error(plan.error(), session->source.relative_path));
                axk::app::filter_export_plan(*plan, selection->closure, selection->volumes);
                session->lease.reset();

                auto staging = sandbox.create_staging_directory("axklib-audio-export");
                if (!staging)
                    return std::unexpected(staging.error());
                TemporaryDirectoryCleanup cleanup{*staging};
                report_progress(context, axk::ProgressPhase::writing, 1U, 4U, "Writing Wave Data");
                auto audio = axk::write_export_audio(*plan, *staging, false, context.cancellation);
                if (!audio)
                    return std::unexpected(core_error(audio.error()));
                std::size_t file_count = audio->written_files.size();
                if (format == "SFZ") {
                    report_progress(context, axk::ProgressPhase::writing, 2U, 4U, "Writing SFZ instruments");
                    auto sfz = axk::write_sfz(*plan, *staging, false, context.cancellation);
                    if (!sfz)
                        return std::unexpected(core_error(sfz.error()));
                    file_count += sfz->written_files.size();
                }
                if (file_count == 0U)
                    return std::unexpected(operation_error("audio_export_empty", "The export produced no files."));

                Json destination;
                try {
                    destination = input.at("destination");
                } catch (const Json::exception &) {
                    return std::unexpected(operation_error("invalid_request", "destination is required"));
                }
                const auto kind = destination.value("kind", std::string{});
                Json result{{"imageId", identity->first},
                            {"revision", identity->second},
                            {"format", format},
                            {"fileCount", file_count}};
                report_progress(context, axk::ProgressPhase::publishing, 3U, 4U, "Publishing audio export");
                if (kind == "WORKSPACE") {
                    DirectoryRef output;
                    try {
                        const auto &value = destination.at("output");
                        output = {value.at("rootId").get<std::string>(), value.at("relativePath").get<std::string>()};
                    } catch (const Json::exception &) {
                        return std::unexpected(
                            operation_error("invalid_request", "workspace destination requires an output directory"));
                    }
                    if (auto published = sandbox.publish_directory(output, false, *staging); !published)
                        return std::unexpected(published.error());
                    result["destination"] = "WORKSPACE";
                    result["output"] = {{"rootId", output.root_id}, {"relativePath", output.relative_path}};
                    result["download"] = nullptr;
                } else if (kind == "DOWNLOAD") {
                    const auto directory_name = destination.value("directoryName", std::string{});
                    if (directory_name.empty() || directory_name == "." || directory_name == ".." ||
                        directory_name.find_first_of("/\\") != std::string::npos) {
                        return std::unexpected(
                            operation_error("invalid_request", "download directory name is invalid"));
                    }
                    auto retained =
                        downloads.create_owned_directory(context.owner_id, *staging, directory_name + ".tar");
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
                report_progress(context, axk::ProgressPhase::publishing, 4U, 4U, "Audio export ready");
                return result;
            });
        if (!bound)
            return bound;
    }
    return {};
}
