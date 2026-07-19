#include "handlers.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "local_operations.hpp"
#include "schema/operations_v1.hpp"
#include "support.hpp"

#include "axklib/alteration.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/application/write_operations.hpp"
#include "axklib/catalog.hpp"
#include "axklib/file_publication.hpp"
#include "axklib/media.hpp"
#include "axklib/utf8.hpp"
#include "axklib/writer.hpp"

namespace axk::cli::commands {
namespace {

int report_application_failure(const axk::app::Error &error) {
    std::cerr << error.code << ": " << error.message << '\n';
    return exit_code(ExitStatus::invalid_request);
}

axk::Result<void> publish_manifest(const std::filesystem::path &output_path, std::string_view contents, bool overwrite,
                                   std::string_view kind) {
    std::error_code filesystem_error;
    if (!overwrite && std::filesystem::exists(output_path, filesystem_error)) {
        return std::unexpected{axk::make_error(axk::ErrorCode::io_open_failed, axk::ErrorCategory::io,
                                               "refusing to replace existing " + std::string{kind} +
                                                   " manifest: " + axk::text::path_to_utf8(output_path))};
    }
    if (filesystem_error) {
        return std::unexpected{axk::make_error(axk::ErrorCode::io_open_failed, axk::ErrorCategory::io,
                                               "could not inspect manifest output path")};
    }
    if (!output_path.parent_path().empty())
        std::filesystem::create_directories(output_path.parent_path(), filesystem_error);
    if (filesystem_error) {
        return std::unexpected{axk::make_error(axk::ErrorCode::io_open_failed, axk::ErrorCategory::io,
                                               "could not create manifest output directory")};
    }
    auto temporary = axk::detail::write_temporary_file(output_path, [&](const axk::detail::TemporaryFileSink &sink) {
        return sink(std::as_bytes(std::span{contents.data(), contents.size()}));
    });
    if (!temporary)
        return std::unexpected{temporary.error()};
    if (auto published = axk::detail::publish_temporary_file(*temporary, output_path, overwrite); !published) {
        std::filesystem::remove(*temporary, filesystem_error);
        return published;
    }
    return {};
}

axk::app::Result<nlohmann::json> invoke_create_image(std::string_view kind, const std::filesystem::path &manifest_path,
                                                     const std::filesystem::path &output_path, bool overwrite) {
    auto prepared = axk::app::prepare_local_build_manifest(kind, manifest_path);
    if (!prepared) {
        return std::unexpected(axk::app::Error{"manifest_invalid", axk::render_error(prepared.error())});
    }
    std::vector<std::filesystem::path> runtime_paths{output_path};
    for (const auto &binding : prepared->bindings)
        runtime_paths.push_back(binding.input_path);
    auto runtime = LocalOperationRuntime::create(runtime_paths);
    if (!runtime)
        return std::unexpected(runtime.error());
    auto output = (*runtime)->file_ref(output_path);
    if (!output)
        return std::unexpected(output.error());
    auto bindings = nlohmann::json::array();
    for (const auto &binding : prepared->bindings) {
        auto source = (*runtime)->file_ref(binding.input_path);
        if (!source)
            return std::unexpected(source.error());
        bindings.push_back(
            {{"manifestPath", binding.manifest_path},
             {"input", {{"fileRef", {{"rootId", source->root_id}, {"relativePath", source->relative_path}}}}}});
    }
    auto plan = (*runtime)->invoke("create.plan",
                                   {{"kind", kind},
                                    {"manifest", {{"inline", std::move(prepared->manifest)}}},
                                    {"inputBindings", std::move(bindings)},
                                    {"output", {{"rootId", output->root_id}, {"relativePath", output->relative_path}}},
                                    {"overwrite", overwrite}});
    if (!plan)
        return std::unexpected(plan.error());
    auto operation = std::string{"create."};
    operation += kind == "HDS" ? "hds" : kind == "FLOPPY" ? "floppy" : "iso";
    return (*runtime)->invoke(operation, {{"planToken", plan->at("planToken")}});
}

axk::app::Result<nlohmann::json> invoke_alteration(const std::filesystem::path &source_path,
                                                   const std::filesystem::path &manifest_path,
                                                   const std::optional<std::filesystem::path> &output_path) {
    auto prepared = axk::app::prepare_local_alteration_manifest(manifest_path);
    if (!prepared) {
        return std::unexpected(axk::app::Error{"manifest_invalid", axk::render_error(prepared.error())});
    }
    std::vector<std::filesystem::path> runtime_paths{source_path};
    if (output_path)
        runtime_paths.push_back(*output_path);
    for (const auto &binding : prepared->bindings)
        runtime_paths.push_back(binding.input_path);
    auto runtime = LocalOperationRuntime::create(runtime_paths);
    if (!runtime)
        return std::unexpected(runtime.error());
    auto source = (*runtime)->file_ref(source_path);
    if (!source)
        return std::unexpected(source.error());
    auto bindings = nlohmann::json::array();
    for (const auto &binding : prepared->bindings) {
        auto input = (*runtime)->file_ref(binding.input_path);
        if (!input)
            return std::unexpected(input.error());
        bindings.push_back(
            {{"manifestPath", binding.manifest_path},
             {"input", {{"fileRef", {{"rootId", input->root_id}, {"relativePath", input->relative_path}}}}}});
    }
    nlohmann::json request = {
        {"source", {{"rootId", source->root_id}, {"relativePath", source->relative_path}}},
        {"manifest", {{"inline", std::move(prepared->manifest)}}},
        {"inputBindings", std::move(bindings)},
    };
    const auto restore_local_audio_paths = [&](nlohmann::json &result) {
        for (auto &operation : result.at("operations")) {
            auto &audio = operation.at("audioImport");
            if (audio.is_null())
                continue;
            const auto &logical = audio.at("sourcePath").get_ref<const std::string &>();
            const auto binding =
                std::ranges::find(prepared->bindings, logical, &axk::app::LocalManifestInputBinding::manifest_path);
            if (binding != prepared->bindings.end())
                audio["sourcePath"] = axk::text::path_to_utf8(binding->input_path);
        }
    };
    if (!output_path) {
        auto inspection = (*runtime)->invoke("alter.inspect", request);
        if (inspection) {
            (*inspection)["applied"] = false;
            restore_local_audio_paths(*inspection);
        }
        return inspection;
    }
    auto output = (*runtime)->file_ref(*output_path);
    if (!output)
        return std::unexpected(output.error());
    request["output"] = {{"rootId", output->root_id}, {"relativePath", output->relative_path}};
    request["overwrite"] = false;
    auto result = (*runtime)->invoke("alter.hds", request);
    if (result)
        restore_local_audio_paths(*result);
    return result;
}

axk::cli::schema::operations_v1::OperationOutput project_operation(const nlohmann::json &operation) {
    auto type = operation.at("type").get<std::string>();
    std::ranges::transform(type, type.begin(), [](char character) {
        return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A')) : character;
    });
    axk::cli::schema::operations_v1::OperationOutput result{
        .id = operation.at("id").get<std::string>(),
        .type = std::move(type),
        .partition_index = operation.at("partitionIndex").get<std::uint8_t>(),
        .volume_name = operation.at("volumeName").get<std::string>(),
        .object_name = operation.at("objectName").get<std::string>(),
        .removed_sfs_ids = operation.at("removedSfsIds").get<std::vector<std::uint32_t>>(),
        .inserted_sfs_ids = operation.at("insertedSfsIds").get<std::vector<std::uint32_t>>(),
        .freed_clusters = operation.at("freedClusters").get<std::uint64_t>(),
        .allocated_clusters = operation.at("allocatedClusters").get<std::uint64_t>(),
        .audio_import = std::nullopt,
    };
    if (!operation.at("audioImport").is_null()) {
        const auto &audio = operation.at("audioImport");
        result.audio_import = axk::cli::schema::operations_v1::AudioImportOutput{
            .source_path_utf8 = audio.at("sourcePath").get<std::string>(),
            .source_format = audio.at("sourceFormat").get<std::string>(),
            .source_subtype = audio.at("sourceSubtype").get<std::string>(),
            .source_channels = audio.at("sourceChannels").get<std::uint8_t>(),
            .source_sample_rate = audio.at("sourceSampleRate").get<std::uint32_t>(),
            .output_sample_rate = audio.at("outputSampleRate").get<std::uint32_t>(),
            .source_sample_width_bits = audio.at("sourceSampleWidthBits").get<std::uint8_t>(),
            .output_sample_width_bits = audio.at("outputSampleWidthBits").get<std::uint8_t>(),
            .output_frames = audio.at("outputFrames").get<std::uint64_t>(),
            .resampled = audio.at("resampled").get<bool>(),
            .quantized = audio.at("quantized").get<bool>(),
            .sample_width_converted = audio.at("sampleWidthConverted").get<bool>(),
            .dither_algorithm = audio.at("ditherAlgorithm").get<std::string>(),
            .split_stereo = audio.at("splitStereo").get<bool>(),
            .clipped_samples = audio.at("clippedSamples").get<std::uint64_t>(),
        };
    }
    return result;
}

} // namespace

int run_create_hds(const std::filesystem::path &manifest_path, const std::filesystem::path &output_path, bool overwrite,
                   bool pretty) {
    static_cast<void>(pretty);
    const auto written = invoke_create_image("HDS", manifest_path, output_path, overwrite);
    if (!written) {
        std::cerr << written.error().message << '\n';
        return exit_code(ExitStatus::invalid_request);
    }
    std::cout << "image=" << axk::text::path_to_utf8(output_path)
              << " size_bytes=" << written->at("sizeBytes").get<std::uint64_t>()
              << " partitions=" << written->at("partitions").size()
              << " objects=" << written->at("objectCount").get<std::size_t>()
              << " unused_tail_sectors=" << written->at("unusedTailSectors").get<std::uint64_t>() << '\n';
    for (const auto &partition : written->at("partitions")) {
        std::cout << "partition=" << partition.at("index").get<unsigned int>() << " name='"
                  << partition.at("name").get_ref<const std::string &>()
                  << "' start_sector=" << partition.at("startSector").get<std::uint32_t>()
                  << " sector_count=" << partition.at("sectorCount").get<std::uint32_t>()
                  << " cluster_count=" << partition.at("clusterCount").get<std::uint32_t>()
                  << " free_kib=" << partition.at("freeKiB").get<std::uint64_t>() << '\n';
    }
    return exit_code(ExitStatus::success);
}

int run_create_media(const std::filesystem::path &manifest_path, const std::filesystem::path &output_path,
                     std::string_view expected_format, bool overwrite, bool pretty) {
    static_cast<void>(pretty);
    const auto service_kind = expected_format == "fat12_floppy" ? std::string_view{"FLOPPY"} : std::string_view{"ISO"};
    const auto written = invoke_create_image(service_kind, manifest_path, output_path, overwrite);
    if (!written) {
        std::cerr << written.error().message << '\n';
        return exit_code(ExitStatus::invalid_request);
    }
    std::cout << "image=" << axk::text::path_to_utf8(output_path) << " format=" << expected_format
              << " size_bytes=" << written->at("sizeBytes").get<std::uint64_t>()
              << " objects=" << written->at("objectCount").get<std::size_t>() << '\n';
    return exit_code(ExitStatus::success);
}

int run_create_manifest(const axk::app::OperationRegistry &registry, std::string_view kind,
                        const std::filesystem::path &output_path, bool overwrite) {
    std::string service_kind;
    if (kind == "hds")
        service_kind = "HDS";
    else if (kind == "floppy")
        service_kind = "FLOPPY";
    else if (kind == "iso")
        service_kind = "ISO";
    if (service_kind.empty()) {
        std::cerr << "manifest kind must be hds, floppy, or iso\n";
        return exit_code(ExitStatus::invalid_request);
    }
    const axk::app::OperationContext context{
        .owner_id = "cli", .request_id = "cli", .cancellation = {}, .progress = nullptr, .display_path = {}};
    const auto manifest = registry.invoke("create.manifest", {{"kind", service_kind}}, context);
    if (!manifest)
        return report_application_failure(manifest.error());
    const auto written =
        publish_manifest(output_path, manifest->at("canonicalJson").get_ref<const std::string &>(), overwrite, "build");
    if (!written) {
        std::cerr << axk::render_error(written.error()) << '\n';
        return exit_code(ExitStatus::invalid_request);
    }
    std::cout << "manifest=" << axk::text::path_to_utf8(output_path) << " kind=" << kind << '\n';
    return exit_code(ExitStatus::success);
}

int run_alter_manifest(const axk::app::OperationRegistry &registry, const std::filesystem::path &output_path,
                       bool overwrite) {
    const axk::app::OperationContext context{
        .owner_id = "cli", .request_id = "cli", .cancellation = {}, .progress = nullptr, .display_path = {}};
    const auto manifest = registry.invoke("alter.manifest", nlohmann::json::object(), context);
    if (!manifest)
        return report_application_failure(manifest.error());
    const auto written = publish_manifest(output_path, manifest->at("canonicalJson").get_ref<const std::string &>(),
                                          overwrite, "alteration");
    if (!written) {
        std::cerr << axk::render_error(written.error()) << '\n';
        return exit_code(ExitStatus::invalid_request);
    }
    std::cout << "manifest=" << axk::text::path_to_utf8(output_path) << " kind=alteration\n";
    return exit_code(ExitStatus::success);
}

int run_alter_hds(const std::filesystem::path &source_path, const std::filesystem::path &manifest_path,
                  const std::optional<std::filesystem::path> &output_path, bool pretty) {
    const auto altered = invoke_alteration(source_path, manifest_path, output_path);
    if (!altered) {
        std::cerr << altered.error().message << '\n';
        return exit_code(ExitStatus::invalid_request);
    }
    axk::cli::schema::operations_v1::AlterationOutput projected{
        .source_path_utf8 = axk::text::path_to_utf8(source_path),
        .output_path_utf8 = output_path ? std::optional{axk::text::path_to_utf8(*output_path)} : std::nullopt,
        .applied = altered->at("applied").get<bool>(),
        .operations = {},
    };
    for (const auto &operation : altered->at("operations"))
        projected.operations.push_back(project_operation(operation));
    const auto serialized = axk::cli::schema::operations_v1::serialize(projected, pretty);
    if (!serialized) {
        std::cerr << axk::render_error(serialized.error()) << '\n';
        return exit_code(ExitStatus::invalid_request);
    }
    std::cout << *serialized << '\n';
    return exit_code(ExitStatus::success);
}

} // namespace axk::cli::commands
