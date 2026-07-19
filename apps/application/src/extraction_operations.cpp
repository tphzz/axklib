#include "axklib/application/extraction_operations.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/application/content_id.hpp"
#include "axklib/application/extraction_selection.hpp"
#include "axklib/application/volume_graph.hpp"
#include "axklib/audio_export.hpp"
#include "axklib/media.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/relationship.hpp"
#include "axklib/utf8.hpp"
#include "axklib/wav_stream.hpp"

namespace {

using Json = nlohmann::json;

struct ExtractionRequest {
    std::vector<axk::app::FileRef> sources;
    axk::app::DirectoryRef destination;
    std::string scope{"file"};
    std::string stereo{"auto"};
    std::vector<std::string> selector_paths;
    bool overwrite{};
    bool strict{};
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
    return {error.code == axk::ErrorCode::operation_cancelled ? "operation_cancelled" : "extraction_failed",
            error.message, std::move(context)};
}

axk::app::Result<ExtractionRequest> parse_request(const Json &input) {
    ExtractionRequest result;
    try {
        if (!input.contains("sources") || !input.at("sources").is_array() || input.at("sources").empty() ||
            input.at("sources").size() > 256U) {
            return std::unexpected(operation_error("invalid_request", "sources must contain 1 to 256 FileRef values"));
        }
        for (const auto &source : input.at("sources")) {
            result.sources.push_back(
                {source.at("rootId").get<std::string>(), source.at("relativePath").get<std::string>()});
        }
        const auto &destination = input.at("destination");
        result.destination = {destination.at("rootId").get<std::string>(),
                              destination.at("relativePath").get<std::string>()};
        result.scope = input.value("scope", std::string{"file"});
        result.stereo = input.value("stereo", std::string{"auto"});
        result.overwrite = input.value("overwrite", false);
        result.strict = input.value("strict", false);
        if (input.contains("selectors")) {
            if (!input.at("selectors").is_array() || input.at("selectors").size() > 1024U)
                return std::unexpected(operation_error("invalid_request", "selectors must be a bounded array"));
            for (const auto &selector : input.at("selectors")) {
                if (selector.is_string())
                    result.selector_paths.push_back(selector.get<std::string>());
                else
                    result.selector_paths.push_back(selector.at("path").get<std::string>());
            }
        }
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "extraction request is malformed"));
    }
    if (result.scope != "file" && result.scope != "volume" && result.scope != "program" && result.scope != "sbac" &&
        result.scope != "sbnk") {
        return std::unexpected(operation_error("unsupported_selection_scope",
                                               "extraction scope must be file, volume, program, sbac, or sbnk"));
    }
    if (result.scope != "file" && result.selector_paths.empty())
        return std::unexpected(operation_error("invalid_request", "selected extraction scope requires selectors"));
    if (result.stereo != "auto" && result.stereo != "none")
        return std::unexpected(operation_error("invalid_request", "stereo must be auto or none"));
    if (std::ranges::any_of(result.selector_paths, [](const auto &path) { return path.empty(); }))
        return std::unexpected(operation_error("invalid_request", "selector paths must not be empty"));
    return result;
}

std::string safe_display_path_name(std::string_view value, std::string_view fallback) {
    auto text = std::string{value};
    const auto first = text.find_first_not_of(" \t\r\n");
    const auto last = text.find_last_not_of(" \t\r\n");
    text = first == std::string::npos ? std::string{fallback} : text.substr(first, last - first + 1U);
    std::size_t stars{};
    while (!text.empty() && text.back() == '*') {
        ++stars;
        text.pop_back();
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
        text.pop_back();
    std::string result;
    bool prior_space{};
    bool prior_underscore{};
    for (const auto character : text) {
        if (character == '<') {
            result += "_lt_";
            prior_underscore = true;
            prior_space = false;
        } else if (character == '>') {
            result += "_gt_";
            prior_underscore = true;
            prior_space = false;
        } else if (std::string_view{"\\/:*?\"|"}.contains(character) || static_cast<unsigned char>(character) < 0x20U) {
            if (!prior_underscore)
                result.push_back('_');
            prior_underscore = true;
            prior_space = false;
        } else if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            if (!result.empty() && !prior_space)
                result.push_back(' ');
            prior_space = true;
            prior_underscore = false;
        } else {
            result.push_back(character);
            prior_space = false;
            prior_underscore = character == '_';
        }
    }
    while (!result.empty() && (result.back() == ' ' || result.back() == '.' || result.back() == '_'))
        result.pop_back();
    while (!result.empty() && (result.front() == ' ' || result.front() == '.' || result.front() == '_'))
        result.erase(result.begin());
    if (result.empty())
        result = fallback;
    if (stars != 0U)
        result += std::format(" ({})", stars + 1U);
    return result;
}

axk::Result<void> retarget_export_plan(axk::ExportPlan &plan, const std::filesystem::path &selection_root,
                                       bool preserve_volume_roots, bool render_stereo,
                                       axk::app::PooledPathAllocator &pooled_paths) {
    for (auto &volume : plan.volumes) {
        volume.relative_root = preserve_volume_roots ? selection_root / volume.relative_root : selection_root;
        std::map<std::string, std::filesystem::path> waveform_paths;
        for (auto &waveform : volume.waveforms) {
            auto pooled = pooled_paths.allocate(volume.relative_root, "physical",
                                                safe_display_path_name(waveform.display_name, "sample"),
                                                axk::audio_internal::WavSource::from_physical(waveform.waveform));
            if (!pooled)
                return std::unexpected{pooled.error()};
            waveform.relative_wav_path = std::move(*pooled);
            waveform_paths.emplace(waveform.object_key, waveform.relative_wav_path);
        }
        for (auto &bank : volume.sample_banks) {
            for (auto &member : bank.members) {
                if (const auto found = waveform_paths.find(member.waveform_key); found != waveform_paths.end())
                    member.relative_wav_path = found->second;
            }
            if (!render_stereo) {
                bank.rendered_wav_path.reset();
                continue;
            }
            if (bank.rendered_wav_path && bank.members.size() == 2U) {
                const auto left = std::ranges::find(volume.waveforms, bank.members[0].waveform_key,
                                                    &axk::PhysicalWaveformExport::object_key);
                const auto right = std::ranges::find(volume.waveforms, bank.members[1].waveform_key,
                                                     &axk::PhysicalWaveformExport::object_key);
                if (left != volume.waveforms.end() && right != volume.waveforms.end()) {
                    auto pooled = pooled_paths.allocate(
                        volume.relative_root, "rendered", safe_display_path_name(bank.display_name, "sample"),
                        axk::audio_internal::WavSource::from_stereo(left->waveform, right->waveform));
                    if (!pooled)
                        return std::unexpected{pooled.error()};
                    bank.rendered_wav_path = std::move(*pooled);
                }
            }
        }
    }
    for (auto &scope : plan.unresolved_wave_data) {
        scope.relative_root = selection_root / scope.relative_root;
        for (auto &waveform : scope.waveforms) {
            auto pooled = pooled_paths.allocate(scope.relative_root, "physical",
                                                safe_display_path_name(waveform.display_name, "wave-data"),
                                                axk::audio_internal::WavSource::from_physical(waveform.waveform));
            if (!pooled)
                return std::unexpected{pooled.error()};
            waveform.relative_wav_path = std::move(*pooled);
        }
    }
    return {};
}

std::filesystem::path selection_root(std::string_view scope, std::string_view selector) {
    if (scope == "file")
        return "file";
    std::filesystem::path result{scope};
    std::string value{selector};
    std::size_t start{};
    while (start <= value.size()) {
        const auto end = value.find('/', start);
        const auto component = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!component.empty())
            result /= safe_display_path_name(component, scope);
        if (end == std::string::npos)
            break;
        start = end + 1U;
    }
    return result;
}

axk::app::Result<void> preflight_selection_roots(const ExtractionRequest &request, const axk::app::Sandbox &sandbox) {
    std::set<std::filesystem::path> roots;
    if (request.scope == "file") {
        for (const auto &source : request.sources) {
            const auto path = sandbox.resolve_file(source);
            if (!path) {
                if (request.strict)
                    return std::unexpected(path.error());
                continue;
            }
            auto root =
                std::filesystem::path{"file"} / safe_display_path_name(axk::text::path_to_utf8(path->stem()), "source");
            if (!roots.insert(std::move(root)).second) {
                return std::unexpected(operation_error(
                    "artifact_collision",
                    "multiple extraction sources resolve to the same output name; rename one source file"));
            }
        }
    } else {
        for (const auto &selector : request.selector_paths) {
            if (!roots.insert(selection_root(request.scope, selector).lexically_normal()).second) {
                return std::unexpected(operation_error(
                    "artifact_collision",
                    "multiple extraction selectors resolve to the same output path; remove the duplicate selector"));
            }
        }
    }
    return {};
}

std::string media_kind_text(axk::MediaKind kind) {
    switch (kind) {
    case axk::MediaKind::sfs:
        return "sfs";
    case axk::MediaKind::fat12_floppy:
        return "fat12_floppy";
    case axk::MediaKind::iso9660:
        return "iso";
    case axk::MediaKind::standalone_object:
        return "standalone_object";
    }
    return "unknown";
}

class DirectoryCleanup {
  public:
    explicit DirectoryCleanup(std::filesystem::path path) : path_(std::move(path)) {}
    ~DirectoryCleanup() {
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }
    void release() { path_.clear(); }

  private:
    std::filesystem::path path_;
};

axk::app::Result<void> write_text(const std::filesystem::path &path, std::string_view value) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return std::unexpected(operation_error("artifact_write_failed", "could not create artifact directory"));
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output << value;
    if (!output)
        return std::unexpected(operation_error("artifact_write_failed", "could not write extraction artifact"));
    return {};
}

axk::app::Result<std::string> sha256_file(const std::filesystem::path &path,
                                          const axk::CancellationToken &cancellation) {
    auto reader = axk::FileReader::open(path);
    if (!reader)
        return std::unexpected(core_error(reader.error()));
    auto digest = axk::package_internal::sha256_reader(**reader, cancellation);
    if (!digest)
        return std::unexpected(core_error(digest.error()));
    return axk::package_internal::hex_digest(*digest);
}

std::string media_type(const std::filesystem::path &path) {
    const auto extension = path.extension().string();
    if (extension == ".wav")
        return "audio/wav";
    if (extension == ".sfz")
        return "application/x-sfz";
    if (extension == ".json")
        return "application/json";
    return "application/octet-stream";
}

using ArtifactOwners = std::map<std::filesystem::path, Json>;

Json artifact_owner(const axk::app::FileRef &source, const axk::VolumeExport &volume, std::string object_type,
                    std::string object_name) {
    return {{"source", {{"rootId", source.root_id}, {"relativePath", source.relative_path}}},
            {"partitionIndex", volume.partition.value},
            {"volumeName", volume.volume_name},
            {"objectType", std::move(object_type)},
            {"objectName", std::move(object_name)}};
}

Json artifact_owner(const axk::app::FileRef &source, const axk::UnresolvedWaveDataExport &scope,
                    std::string object_type, std::string object_name) {
    return {{"source", {{"rootId", source.root_id}, {"relativePath", source.relative_path}}},
            {"partitionIndex", scope.partition.value},
            {"volumeName", "Unresolved Wave Data"},
            {"objectType", std::move(object_type)},
            {"objectName", std::move(object_name)}};
}

void add_artifact_owner(ArtifactOwners &owners, std::filesystem::path path, Json owner) {
    auto &entries = owners[path.lexically_normal()];
    if (!entries.is_array())
        entries = Json::array();
    if (std::ranges::find(entries, owner) == entries.end())
        entries.push_back(std::move(owner));
}

bool has_sfz_region(const axk::VolumeExport &volume, const axk::SampleBankExport &bank) {
    if (bank.rendered_wav_path && !bank.members.empty()) {
        return std::ranges::find(volume.waveforms, bank.members.front().waveform_key,
                                 &axk::PhysicalWaveformExport::object_key) != volume.waveforms.end();
    }
    return std::ranges::any_of(bank.members, [&](const auto &member) {
        return member.quality == axk::RelationshipQuality::known &&
               std::ranges::find(volume.waveforms, member.waveform_key, &axk::PhysicalWaveformExport::object_key) !=
                   volume.waveforms.end();
    });
}

Json object_owner(Json owner, std::string object_type, std::string object_name) {
    owner["objectType"] = std::move(object_type);
    owner["objectName"] = std::move(object_name);
    return owner;
}

axk::app::Result<void> publish_directory(const std::filesystem::path &staging, const std::filesystem::path &destination,
                                         bool overwrite) {
    std::error_code error;
    if (!std::filesystem::exists(destination, error)) {
        std::filesystem::rename(staging, destination, error);
        if (!error)
            return {};
        return std::unexpected(operation_error("artifact_publish_failed", "could not publish extraction directory"));
    }
    if (!overwrite) {
        if (!std::filesystem::is_empty(destination, error) || error)
            return std::unexpected(operation_error("artifact_exists", "extraction destination already exists"));
        std::filesystem::remove(destination, error);
        if (error)
            return std::unexpected(
                operation_error("artifact_publish_failed", "could not reserve empty extraction destination"));
        std::filesystem::rename(staging, destination, error);
        if (!error)
            return {};
        return std::unexpected(operation_error("artifact_publish_failed", "could not publish extraction directory"));
    }
    const auto backup = axk::text::temporary_sibling(destination);
    if (!backup)
        return std::unexpected(operation_error("artifact_publish_failed", "could not name extraction backup"));
    std::filesystem::rename(destination, *backup, error);
    if (error)
        return std::unexpected(operation_error("artifact_publish_failed", "could not reserve extraction destination"));
    std::filesystem::rename(staging, destination, error);
    if (error) {
        std::error_code restore_error;
        std::filesystem::rename(*backup, destination, restore_error);
        return std::unexpected(operation_error("artifact_publish_failed", "could not publish extraction directory"));
    }
    std::filesystem::remove_all(*backup, error);
    return {};
}

axk::app::Result<Json> extract(const Json &input, const axk::app::OperationContext &context,
                               const axk::app::Sandbox &sandbox, bool sfz) {
    auto request = parse_request(input);
    if (!request)
        return std::unexpected(request.error());
    auto destination = sandbox.resolve_output_directory(request->destination, request->overwrite);
    if (!destination)
        return std::unexpected(destination.error());
    if (const auto preflight = preflight_selection_roots(*request, sandbox); !preflight)
        return std::unexpected(preflight.error());
    const auto staging_result = axk::text::temporary_sibling(*destination);
    if (!staging_result)
        return std::unexpected(operation_error("artifact_write_failed", "could not name extraction staging"));
    const auto staging = *staging_result;
    std::error_code error;
    std::filesystem::create_directory(staging, error);
    if (error)
        return std::unexpected(
            operation_error("artifact_write_failed", "could not create extraction staging directory"));
    DirectoryCleanup cleanup{staging};

    axk::ExportPlan combined;
    std::map<std::filesystem::path, std::string> volume_graphs;
    std::map<std::filesystem::path, std::string> unresolved_graphs;
    Json warnings = Json::array();
    ArtifactOwners artifact_owners;
    std::vector<Json> combined_volume_owners;
    std::size_t load_error_count{};
    axk::app::PooledPathAllocator pooled_paths;
    const auto selectors = request->scope == "file" ? std::vector<std::string>{""} : request->selector_paths;
    std::vector<bool> selectors_found(selectors.size(), request->scope == "file");
    for (std::size_t source_index = 0; source_index < request->sources.size(); ++source_index) {
        if (context.progress) {
            context.progress->report({axk::ProgressPhase::reading, source_index, request->sources.size(),
                                      "Reading " + request->sources[source_index].relative_path, std::nullopt});
        }
        const auto &source = request->sources[source_index];
        const auto source_display = context.display_path ? context.display_path(source) : source.relative_path;
        auto source_path = sandbox.resolve_file(source);
        if (!source_path) {
            if (request->strict)
                return std::unexpected(source_path.error());
            ++load_error_count;
            warnings.push_back({{"code", source_path.error().code},
                                {"message", source_path.error().message},
                                {"source", source_display}});
            continue;
        }
        auto media = axk::open_media(*source_path, context.cancellation);
        if (!media) {
            if (request->strict)
                return std::unexpected(core_error(media.error(), source_display));
            ++load_error_count;
            warnings.push_back(
                {{"code", "source_failed"}, {"message", media.error().message}, {"source", source_display}});
            continue;
        }
        auto inventory = axk::build_media_inventory(*media, axk::MediaObjectReadMode::decoded_metadata,
                                                    64U * 1024U * 1024U, context.cancellation);
        if (!inventory) {
            if (request->strict)
                return std::unexpected(core_error(inventory.error(), source_display));
            ++load_error_count;
            warnings.push_back(
                {{"code", "source_failed"}, {"message", inventory.error().message}, {"source", source_display}});
            continue;
        }
        auto graph = axk::build_relationship_graph(inventory->catalog);
        const auto tree = axk::build_content_tree(*media, inventory->catalog, graph, false);
        for (std::size_t selector_index = 0; selector_index < selectors.size(); ++selector_index) {
            const auto &selector = selectors[selector_index];
            std::optional<axk::app::ExtractionSelection> selection;
            if (request->scope != "file") {
                auto resolved = axk::app::resolve_extraction_selection(media->kind(), tree, request->scope, selector);
                if (!resolved && resolved.error().code == "selector_not_found")
                    continue;
                if (!resolved)
                    return std::unexpected(resolved.error());
                selection = std::move(*resolved);
            }
            selectors_found[selector_index] = true;
            auto plan = axk::build_export_plan(*media, inventory->catalog, graph, context.cancellation);
            if (!plan)
                return std::unexpected(core_error(plan.error(), source_display));
            if (selection) {
                axk::app::filter_export_plan(*plan, graph, request->scope, selector, selection->object_key);
            }
            auto root = selection_root(request->scope, selector);
            if (request->scope == "file")
                root /= safe_display_path_name(axk::text::path_to_utf8(source_path->stem()), "source");
            if (auto retargeted = retarget_export_plan(*plan, root, request->scope == "file", request->stereo == "auto",
                                                       pooled_paths);
                !retargeted) {
                return std::unexpected(core_error(retargeted.error(), source_display));
            }
            for (const auto &volume : plan->volumes) {
                auto volume_owner = artifact_owner(source, volume, "VOLUME", volume.volume_name);
                combined_volume_owners.push_back(volume_owner);
                const auto graph_path = volume.relative_root / "volume.axklib.json";
                add_artifact_owner(artifact_owners, graph_path, volume_owner);
                for (const auto &waveform : volume.waveforms) {
                    add_artifact_owner(artifact_owners, volume.relative_root / waveform.relative_wav_path,
                                       artifact_owner(source, volume, "SMPL", waveform.display_name));
                }
                for (const auto &bank : volume.sample_banks) {
                    if (bank.rendered_wav_path) {
                        add_artifact_owner(artifact_owners, volume.relative_root / *bank.rendered_wav_path,
                                           artifact_owner(source, volume, "SBNK", bank.display_name));
                    }
                }
                auto serialized = axk::app::serialize_volume_graph(volume, graph, std::filesystem::path{source_display},
                                                                   media_kind_text(media->kind()));
                if (!serialized)
                    return std::unexpected(core_error(serialized.error(), source_display));
                if (const auto existing = volume_graphs.find(graph_path); existing != volume_graphs.end()) {
                    if (existing->second == *serialized)
                        continue;
                    return std::unexpected(
                        operation_error("artifact_collision", "distinct volume graphs share output path: " +
                                                                  axk::text::path_to_utf8(graph_path)));
                }
                volume_graphs.emplace(graph_path, std::move(*serialized));
            }
            for (const auto &scope : plan->unresolved_wave_data) {
                const auto graph_path = scope.relative_root / "unresolved.axklib.json";
                add_artifact_owner(artifact_owners, graph_path,
                                   artifact_owner(source, scope, "SMPL", "Unresolved Wave Data"));
                for (const auto &waveform : scope.waveforms) {
                    add_artifact_owner(artifact_owners, scope.relative_root / waveform.relative_wav_path,
                                       artifact_owner(source, scope, "SMPL", waveform.display_name));
                }
                auto serialized = axk::app::serialize_unresolved_wave_data_graph(
                    scope, std::filesystem::path{source_display}, media_kind_text(media->kind()));
                if (!serialized)
                    return std::unexpected(core_error(serialized.error(), source_display));
                if (const auto existing = unresolved_graphs.find(graph_path); existing != unresolved_graphs.end()) {
                    if (existing->second == *serialized)
                        continue;
                    return std::unexpected(operation_error("artifact_collision",
                                                           "distinct unresolved Wave Data graphs share output path: " +
                                                               axk::text::path_to_utf8(graph_path)));
                }
                unresolved_graphs.emplace(graph_path, std::move(*serialized));
            }
            std::ranges::move(plan->decode_errors, std::back_inserter(combined.decode_errors));
            std::ranges::move(plan->volumes, std::back_inserter(combined.volumes));
            std::ranges::move(plan->unresolved_wave_data, std::back_inserter(combined.unresolved_wave_data));
        }
    }
    for (std::size_t index = 0; index < selectors.size(); ++index) {
        if (!selectors_found[index]) {
            return std::unexpected(operation_error("selector_not_found", "selector path was not found for " +
                                                                             request->scope + ": " + selectors[index]));
        }
    }
    if (context.progress)
        context.progress->report(
            {axk::ProgressPhase::exporting, 0U, std::nullopt, "Writing exact audio", std::nullopt});
    auto audio = axk::write_export_audio(combined, staging, false, context.cancellation);
    if (!audio)
        return std::unexpected(core_error(audio.error(), request->destination.relative_path));
    std::vector<std::filesystem::path> written = audio->written_files;
    std::size_t sfz_count{};
    if (sfz) {
        if (const auto checked = context.cancellation.check(); !checked)
            return std::unexpected(core_error(checked.error()));
        auto instruments = axk::write_sfz(combined, staging, false);
        if (!instruments)
            return std::unexpected(core_error(instruments.error(), request->destination.relative_path));
        sfz_count = instruments->written_files.size();
        auto instrument = instruments->written_files.begin();
        for (std::size_t volume_index = 0; volume_index < combined.volumes.size(); ++volume_index) {
            const auto &volume = combined.volumes[volume_index];
            std::set<std::string> grouped;
            for (const auto &group : volume.sample_bank_groups) {
                const auto has_region = std::ranges::any_of(group.member_bank_keys, [&](const auto &key) {
                    const auto bank = std::ranges::find(volume.sample_banks, key, &axk::SampleBankExport::object_key);
                    return bank != volume.sample_banks.end() && has_sfz_region(volume, *bank);
                });
                grouped.insert(group.member_bank_keys.begin(), group.member_bank_keys.end());
                if (has_region && instrument != instruments->written_files.end()) {
                    add_artifact_owner(artifact_owners, instrument->lexically_relative(staging),
                                       object_owner(combined_volume_owners[volume_index], "SBAC", group.display_name));
                    ++instrument;
                }
            }
            for (const auto &bank : volume.sample_banks) {
                if (!grouped.contains(bank.object_key) && has_sfz_region(volume, bank) &&
                    instrument != instruments->written_files.end()) {
                    add_artifact_owner(artifact_owners, instrument->lexically_relative(staging),
                                       object_owner(combined_volume_owners[volume_index], "SBNK", bank.display_name));
                    ++instrument;
                }
            }
        }
        if (instrument != instruments->written_files.end())
            return std::unexpected(operation_error("artifact_manifest_failed", "could not assign SFZ artifact owner"));
        std::ranges::move(instruments->written_files, std::back_inserter(written));
    }
    for (const auto &[relative, graph] : volume_graphs) {
        auto graph_path = staging / relative;
        if (auto saved = write_text(graph_path, graph + "\n"); !saved)
            return std::unexpected(saved.error());
        written.push_back(std::move(graph_path));
    }
    for (const auto &[relative, graph] : unresolved_graphs) {
        auto graph_path = staging / relative;
        if (auto saved = write_text(graph_path, graph + "\n"); !saved)
            return std::unexpected(saved.error());
        written.push_back(std::move(graph_path));
    }
    for (const auto &decode_error : combined.decode_errors)
        warnings.push_back({{"code", "waveform_skipped"}, {"message", decode_error}});

    std::ranges::sort(written, {},
                      [&](const auto &path) { return axk::text::path_to_utf8(path.lexically_relative(staging)); });
    auto artifacts = Json::array();
    for (const auto &path : written) {
        const auto relative = path.lexically_relative(staging);
        auto digest = sha256_file(path, context.cancellation);
        if (!digest)
            return std::unexpected(digest.error());
        const auto size = std::filesystem::file_size(path, error);
        if (error)
            return std::unexpected(operation_error("artifact_read_failed", "could not inspect extraction artifact"));
        const auto owner = artifact_owners.find(relative.lexically_normal());
        const auto owners = owner == artifact_owners.end() ? Json::array() : owner->second;
        artifacts.push_back(
            {{"fileRef",
              {{"rootId", request->destination.root_id},
               {"relativePath", request->destination.relative_path + "/" + axk::text::path_to_utf8(relative)}}},
             {"relativePath", axk::text::path_to_utf8(relative)},
             {"mediaType", media_type(path)},
             {"sizeBytes", size},
             {"sha256", *digest},
             {"owners", owners}});
    }
    if (const auto checked = context.cancellation.check(); !checked)
        return std::unexpected(core_error(checked.error()));
    if (auto published = publish_directory(staging, *destination, request->overwrite); !published)
        return std::unexpected(published.error());
    cleanup.release();
    const auto waveform_count =
        std::accumulate(combined.volumes.begin(), combined.volumes.end(), std::size_t{},
                        [](std::size_t count, const auto &volume) { return count + volume.waveforms.size(); });
    const auto unresolved_waveform_count =
        std::accumulate(combined.unresolved_wave_data.begin(), combined.unresolved_wave_data.end(), std::size_t{},
                        [](std::size_t count, const auto &scope) { return count + scope.waveforms.size(); });
    return Json{{"schemaVersion", "1.0"},
                {"mode", sfz ? "SFZ" : "WAV"},
                {"destination",
                 {{"rootId", request->destination.root_id}, {"relativePath", request->destination.relative_path}}},
                {"artifactCount", artifacts.size()},
                {"waveformCount", waveform_count + unresolved_waveform_count},
                {"writtenFileCount", written.size()},
                {"selectionGraphCount", volume_graphs.size() + unresolved_graphs.size()},
                {"sfzFileCount", sfz_count},
                {"decodeErrorCount", combined.decode_errors.size()},
                {"loadErrorCount", load_error_count},
                {"artifacts", std::move(artifacts)},
                {"warnings", std::move(warnings)}};
}

} // namespace

axk::app::Result<void> axk::app::bind_extraction_operations(OperationRegistry &registry, const Sandbox &sandbox) {
    if (!registry.is_implemented("extract.wav")) {
        auto bound = registry.bind("extract.wav", [&sandbox](const Json &input, const OperationContext &context) {
            return extract(input, context, sandbox, false);
        });
        if (!bound)
            return bound;
    }
    if (!registry.is_implemented("extract.sfz")) {
        auto bound = registry.bind("extract.sfz", [&sandbox](const Json &input, const OperationContext &context) {
            return extract(input, context, sandbox, true);
        });
        if (!bound)
            return bound;
    }
    return {};
}
