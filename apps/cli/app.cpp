#include "app.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include <CLI/CLI.hpp>

#include "commands/handlers.hpp"
#include "requests.hpp"
#include "writer_commands.hpp"

#include "axklib/utf8.hpp"
#include "axklib/version.hpp"

int axk::cli::run(int argc, char **argv) {
    using namespace axk::cli::commands;
    for (int index = 0; index < argc; ++index) {
        if (argv[index] == nullptr || !axk::text::is_valid_utf8(argv[index])) {
            std::cerr << "error: command-line argument is not valid UTF-8\n";
            return 2;
        }
    }
    CLI::App app{"Yamaha A-series disk image and sampler object tooling"};
    app.set_version_flag("--version", std::string{axk::version()});

    axk::cli::InfoRequest info_request;
    auto *info = app.add_subcommand("info", "summarize supported axklib containers");
    info->add_option("paths", info_request.paths, "input files or directories")->required()->expected(1, -1);
    info->add_flag("--strict", info_request.strict, "stop after the first load error");
    info->add_option("--format", info_request.format, "tree, json, summary, or paths")
        ->check(CLI::IsMember({"tree", "json", "summary", "paths"}));
    info->add_option("--max-depth", info_request.max_depth, "maximum rendered tree depth");
    info->add_flag("--show-quality", info_request.show_quality, "show quality labels");
    info->add_flag("--show-unresolved", info_request.show_unresolved, "show unresolved relationship notes");
    info->add_flag("--show-default-programs", info_request.show_default_programs, "show all 128 Program slots");

    axk::cli::ObjectsRequest objects_request;
    auto *objects = app.add_subcommand("objects", "decode current sampler objects as JSON");
    objects->add_option("paths", objects_request.paths, "input files, directories, or expanded globs")
        ->required()
        ->expected(1, -1);
    objects->add_option("-o,--output-dir", objects_request.output_directory, "directory for object reports")
        ->required();
    objects->add_option("--object-type", objects_request.object_type, "filter SMPL/SBNK/SBAC/PROG/SEQU/PRF3")
        ->check(CLI::IsMember({"SMPL", "SBNK", "SBAC", "PROG", "SEQU", "PRF3"}));
    objects->add_flag("--with-payloads", objects_request.with_payloads, "include decoded payload fields");
    objects->add_flag("--strict", objects_request.strict, "stop after the first load error");
    objects->add_flag("--overwrite", objects_request.overwrite, "allow writing into a non-empty output directory");
    objects->add_flag("--pretty", objects_request.pretty, "indent stdout JSON");

    axk::cli::RelationshipsRequest relationships_request;
    auto *relationships = app.add_subcommand("relationships", "resolve sampler object links as JSON");
    relationships->add_option("paths", relationships_request.paths, "input files, directories, or expanded globs")
        ->required()
        ->expected(1, -1);
    relationships
        ->add_option("-o,--output-dir", relationships_request.output_directory, "directory for relationship reports")
        ->required();
    relationships->add_flag("--overwrite", relationships_request.overwrite,
                            "allow writing into a non-empty output directory");
    relationships->add_option("--mono-dir", relationships_request.mono_directory,
                              "mono exact-export sidecar directory");

    axk::cli::InventoryRequest inventory_request;
    auto *inventory = app.add_subcommand("inventory", "decode object inventory through the model");
    inventory->add_option("paths", inventory_request.paths, "input files or directories")->required()->expected(1, -1);
    inventory->add_option("-o,--output-dir", inventory_request.output_directory, "directory for inventory reports")
        ->required();
    inventory->add_flag("--strict", inventory_request.strict, "stop after the first load error");
    inventory->add_flag("--overwrite", inventory_request.overwrite, "allow writing into a non-empty output directory");

    axk::cli::CoverageRequest coverage_request;
    auto *coverage = app.add_subcommand("coverage", "summarize current relationship coverage");
    coverage->add_option("paths", coverage_request.paths, "input files or directories")->required()->expected(1, -1);
    coverage->add_option("-o,--output-dir", coverage_request.output_directory, "directory for coverage reports")
        ->required();
    coverage->add_flag("--overwrite", coverage_request.overwrite, "allow writing into a non-empty output directory");

    axk::cli::CorpusAuditRequest corpus_request;
    auto *corpus = app.add_subcommand("corpus", "run corpus-level workflows");
    auto *corpus_audit =
        corpus->add_subcommand("audit", "run inventory, validation, relationship, and waveform smoke checks");
    corpus_audit->add_option("paths", corpus_request.paths, "input files or directories")->required()->expected(1, -1);
    corpus_audit->add_option("-o,--output-dir", corpus_request.output_directory, "directory for corpus audit reports")
        ->required();
    corpus_audit->add_option("--policy", corpus_request.policy, "validation policy")
        ->check(CLI::IsMember({"normal", "strict", "salvage-aware"}));
    corpus_audit->add_option("--wave-smoke-limit", corpus_request.wave_smoke_limit,
                             "maximum decoded waveforms counted per container");
    corpus_audit->add_flag("--skip-wave-smoke", corpus_request.skip_wave_smoke, "skip waveform decode checks");
    corpus_audit->add_flag("--overwrite", corpus_request.overwrite, "allow writing into a non-empty output directory");

    axk::cli::ExtractRequest extract_wav_request;
    axk::cli::ExtractRequest extract_sfz_request;
    extract_sfz_request.sfz = true;
    auto *extract = app.add_subcommand("extract", "extract data from supported containers");
    const auto configure_extract = [](CLI::App &command, axk::cli::ExtractRequest &request) {
        command.add_option("scope", request.scope, "selection scope")
            ->required()
            ->check(CLI::IsMember({"file", "volume", "program", "sbac", "sbnk"}));
        command.add_option("paths", request.paths, "input files or directories")->required()->expected(1, -1);
        command.add_option("-o,--output-dir", request.output_directory, "export output directory")->required();
        command.add_option("--path", request.selector_paths, "selector path from info --format paths");
        command.add_option("--stereo", request.stereo, "stereo export policy")->check(CLI::IsMember({"none", "auto"}));
        command.add_flag("--overwrite", request.overwrite, "replace existing export files");
        command.add_flag("--strict", request.strict, "stop after the first load error");
        command.add_option("--progress", request.progress, "progress display policy")
            ->check(CLI::IsMember({"auto", "always", "never"}));
    };
    auto *extract_wav_nested = extract->add_subcommand("wav", "export targeted WAVs to a shared sample pool");
    configure_extract(*extract_wav_nested, extract_wav_request);
    auto *extract_sfz_nested = extract->add_subcommand("sfz", "export targeted WAVs and generate SFZ files");
    configure_extract(*extract_sfz_nested, extract_sfz_request);

    axk::cli::PackageExportRequest package_export_request;
    axk::cli::PackageReadRequest package_inspect_request;
    axk::cli::PackageReadRequest package_verify_request;
    axk::cli::PackageImportRequest package_plan_request;
    axk::cli::PackageImportRequest package_import_request;
    package_import_request.apply = true;
    auto *package = app.add_subcommand("package", "move self-contained sampler object graphs");
    auto *package_export = package->add_subcommand("export", "export one portable package");
    package_export->add_option("source", package_export_request.source, "source sampler image")->required();
    package_export
        ->add_option("--root", package_export_request.roots, "root selector: volume or program|sbac|sbnk|smpl=NAME")
        ->required()
        ->expected(1, -1);
    package_export->add_option("--partition", package_export_request.partition_index, "numeric source partition index");
    package_export->add_option("--group", package_export_request.group_name, "source partition or CD-ROM group label");
    package_export->add_option("--volume", package_export_request.volume_name, "source volume label");
    package_export->add_option("-o,--output", package_export_request.output, "package filename or suffix-free stem")
        ->required();
    package_export->add_option("--format", package_export_request.format, "summary or json")
        ->check(CLI::IsMember({"summary", "json"}));
    package_export->add_flag("--overwrite", package_export_request.overwrite, "replace an existing package atomically");

    const auto configure_package_read = [](CLI::App &command, axk::cli::PackageReadRequest &request) {
        command.add_option("package", request.package, "portable package file")->required();
        command.add_option("--format", request.format, "summary or json")->check(CLI::IsMember({"summary", "json"}));
    };
    auto *package_inspect =
        package->add_subcommand("inspect", "inspect bounded package metadata without hashing payloads");
    configure_package_read(*package_inspect, package_inspect_request);
    auto *package_verify = package->add_subcommand("verify", "verify a package");
    configure_package_read(*package_verify, package_verify_request);

    const auto configure_package_import = [](CLI::App &command, axk::cli::PackageImportRequest &request) {
        command.add_option("target", request.target, "target sampler image")->required();
        command.add_option("packages", request.packages, "portable package files")->required()->expected(1, -1);
        command
            .add_option("--destination", request.destinations,
                        "JSON destination object with package, root, and sampler scope")
            ->required()
            ->expected(1, -1);
        command.add_option("--rename-map", request.rename_map, "JSON array of explicit node renames");
        command.add_option("--format", request.format, "summary or json")->check(CLI::IsMember({"summary", "json"}));
    };
    auto *package_plan = package->add_subcommand("plan-import", "plan a package import");
    configure_package_import(*package_plan, package_plan_request);
    auto *package_import = package->add_subcommand("import", "atomically import packages");
    configure_package_import(*package_import, package_import_request);
    package_import->add_option("-o,--output", package_import_request.output, "output sampler image")->required();
    package_import->add_flag("--overwrite", package_import_request.overwrite,
                             "atomically replace an existing output image");

    axk::cli::OrphansRequest orphans_request;
    auto *orphans = app.add_subcommand("orphans", "classify physical waveform ownership as JSON");
    orphans->add_option("paths", orphans_request.paths, "input HDS image paths")->required()->expected(1, -1);
    orphans->add_option("-o,--output-dir", orphans_request.output_directory, "directory for orphan reports")
        ->required();
    orphans->add_flag("--overwrite", orphans_request.overwrite, "allow writing into a non-empty output directory");

    axk::cli::ValidateRequest validate_request;
    auto *validate = app.add_subcommand("validate", "validate container and sampler organization");
    validate->add_option("paths", validate_request.paths, "input files or directories")->expected(0, -1);
    validate->add_option("--exports", validate_request.exports, "export directory to validate");
    validate->add_option("-o,--output-dir", validate_request.output_directory, "directory for validation reports")
        ->required();
    validate->add_option("--policy", validate_request.policy, "validation policy")
        ->check(CLI::IsMember({"normal", "strict", "salvage-aware"}));
    validate->add_flag("--strict", validate_request.strict, "alias for --policy strict");
    validate->add_flag("--overwrite", validate_request.overwrite, "allow writing into a non-empty output directory");

    WriterCommandState writer_commands;
    register_writer_commands(app, writer_commands);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError &error) {
        return app.exit(error);
    }
    if (*info) {
        return run_info_request(info_request);
    }
    if (*objects)
        return run_objects_request(objects_request);
    if (*relationships)
        return run_relationships_request(relationships_request);
    if (*inventory)
        return run_inventory_request(inventory_request);
    if (*coverage)
        return run_coverage_request(coverage_request);
    if (*corpus_audit)
        return run_corpus_audit_request(corpus_request);
    if (*extract_wav_nested)
        return run_extract_request(extract_wav_request);
    if (*extract_sfz_nested)
        return run_extract_request(extract_sfz_request);
    if (*package_export)
        return run_package_export(package_export_request);
    if (*package_inspect)
        return run_package_inspect(package_inspect_request, false);
    if (*package_verify)
        return run_package_inspect(package_verify_request, true);
    if (*package_plan)
        return run_package_import(package_plan_request);
    if (*package_import)
        return run_package_import(package_import_request);
    if (*orphans)
        return run_orphans_request(orphans_request);
    if (*validate) {
        if (validate_request.strict)
            validate_request.policy = "strict";
        return run_validate_request(validate_request);
    }
    if (*writer_commands.create_hds) {
        return run_create_hds(writer_commands.create_manifest, writer_commands.create_output,
                              writer_commands.create_overwrite, writer_commands.create_pretty);
    }
    if (*writer_commands.create_floppy) {
        return run_create_media(writer_commands.create_manifest, writer_commands.create_output, "fat12_floppy",
                                writer_commands.create_overwrite, writer_commands.create_pretty);
    }
    if (*writer_commands.create_iso) {
        return run_create_media(writer_commands.create_manifest, writer_commands.create_output, "iso9660",
                                writer_commands.create_overwrite, writer_commands.create_pretty);
    }
    if (*writer_commands.create_manifest_command)
        return run_create_manifest(writer_commands.create_manifest_kind, writer_commands.create_manifest_output,
                                   writer_commands.create_manifest_overwrite);
    if (*writer_commands.alter_manifest_command)
        return run_alter_manifest(writer_commands.alter_manifest_output, writer_commands.alter_manifest_overwrite);
    if (*writer_commands.alter_hds) {
        const auto output = !writer_commands.alter_output.empty()
                                ? std::optional<std::filesystem::path>{writer_commands.alter_output}
                                : std::nullopt;
        return run_alter_hds(writer_commands.alter_source, writer_commands.alter_manifest, output,
                             writer_commands.alter_pretty);
    }
    std::cout << app.help();
    return 0;
}
