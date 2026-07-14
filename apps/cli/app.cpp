#include "app.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include <CLI/CLI.hpp>

#include "commands/handlers.hpp"
#include "requests.hpp"

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
  info->add_option("paths", info_request.paths, "input files or directories")
      ->required()
      ->expected(1, -1);
  info->add_flag("--strict", info_request.strict, "stop after the first load error");
  info->add_option("--format", info_request.format, "tree, json, summary, or paths")
      ->check(CLI::IsMember({"tree", "json", "summary", "paths"}));
  info->add_option("--max-depth", info_request.max_depth, "maximum rendered tree depth");
  info->add_flag("--show-quality", info_request.show_quality, "show quality labels");
  info->add_flag("--show-unresolved", info_request.show_unresolved,
                 "show unresolved relationship notes");
  info->add_flag("--show-default-programs", info_request.show_default_programs,
                 "show all 128 Program slots");

  axk::cli::ObjectsRequest objects_request;
  auto *objects = app.add_subcommand("objects", "decode current sampler objects as JSON");
  objects->add_option("paths", objects_request.paths, "input files, directories, or expanded globs")
      ->required()
      ->expected(1, -1);
  objects
      ->add_option("-o,--output-dir", objects_request.output_directory,
                   "directory for object reports")
      ->required();
  objects
      ->add_option("--object-type", objects_request.object_type,
                   "filter SMPL/SBNK/SBAC/PROG/SEQU/PRF3")
      ->check(CLI::IsMember({"SMPL", "SBNK", "SBAC", "PROG", "SEQU", "PRF3"}));
  objects->add_flag("--with-payloads", objects_request.with_payloads,
                    "include decoded payload fields");
  objects->add_flag("--strict", objects_request.strict, "stop after the first load error");
  objects->add_flag("--overwrite", objects_request.overwrite,
                    "allow writing into a non-empty output directory");
  objects->add_flag("--pretty", objects_request.pretty, "indent stdout JSON");

  std::filesystem::path object_json_path;
  bool object_json_pretty = false;
  auto *object_json = app.add_subcommand("object-json", "emit canonical object semantics");
  object_json->group("");
  object_json->add_option("image", object_json_path, "input HDA/HDS image")->required();
  object_json->add_flag("--pretty", object_json_pretty, "indent JSON output");

  axk::cli::RelationshipsRequest relationships_request;
  auto *relationships = app.add_subcommand("relationships", "resolve sampler object links as JSON");
  relationships
      ->add_option("paths", relationships_request.paths,
                   "input files, directories, or expanded globs")
      ->required()
      ->expected(1, -1);
  relationships
      ->add_option("-o,--output-dir", relationships_request.output_directory,
                   "directory for relationship reports")
      ->required();
  relationships->add_flag("--overwrite", relationships_request.overwrite,
                          "allow writing into a non-empty output directory");
  relationships->add_option("--mono-dir", relationships_request.mono_directory,
                            "mono exact-export sidecar directory");

  axk::cli::InventoryRequest inventory_request;
  auto *inventory = app.add_subcommand("inventory", "decode object inventory through the model");
  inventory->add_option("paths", inventory_request.paths, "input files or directories")
      ->required()
      ->expected(1, -1);
  inventory
      ->add_option("-o,--output-dir", inventory_request.output_directory,
                   "directory for inventory reports")
      ->required();
  inventory->add_flag("--strict", inventory_request.strict, "stop after the first load error");
  inventory->add_flag("--overwrite", inventory_request.overwrite,
                      "allow writing into a non-empty output directory");

  axk::cli::CoverageRequest coverage_request;
  auto *coverage = app.add_subcommand("coverage", "summarize current relationship coverage");
  coverage->add_option("paths", coverage_request.paths, "input files or directories")
      ->required()
      ->expected(1, -1);
  coverage
      ->add_option("-o,--output-dir", coverage_request.output_directory,
                   "directory for coverage reports")
      ->required();
  coverage->add_flag("--overwrite", coverage_request.overwrite,
                     "allow writing into a non-empty output directory");

  axk::cli::CorpusAuditRequest corpus_request;
  auto *corpus = app.add_subcommand("corpus", "run corpus-level workflows");
  auto *corpus_audit = corpus->add_subcommand(
      "audit", "run inventory, validation, relationship, and waveform smoke checks");
  corpus_audit->add_option("paths", corpus_request.paths, "input files or directories")
      ->required()
      ->expected(1, -1);
  corpus_audit
      ->add_option("-o,--output-dir", corpus_request.output_directory,
                   "directory for corpus audit reports")
      ->required();
  corpus_audit->add_option("--policy", corpus_request.policy, "validation policy")
      ->check(CLI::IsMember({"normal", "strict", "salvage-aware"}));
  corpus_audit->add_option("--wave-smoke-limit", corpus_request.wave_smoke_limit,
                           "maximum decoded waveforms counted per container");
  corpus_audit->add_flag("--skip-wave-smoke", corpus_request.skip_wave_smoke,
                         "skip waveform decode checks");
  corpus_audit->add_flag("--overwrite", corpus_request.overwrite,
                         "allow writing into a non-empty output directory");

  axk::cli::ExtractRequest extract_wav_request;
  axk::cli::ExtractRequest extract_sfz_request;
  extract_sfz_request.sfz = true;
  auto *extract = app.add_subcommand("extract", "extract data from supported containers");
  const auto configure_extract = [](CLI::App &command, axk::cli::ExtractRequest &request) {
    command.add_option("scope", request.scope, "selection scope")
        ->required()
        ->check(CLI::IsMember({"file", "volume", "program", "sbac", "sbnk"}));
    command.add_option("paths", request.paths, "input files or directories")
        ->required()
        ->expected(1, -1);
    command.add_option("-o,--output-dir", request.output_directory, "export output directory")
        ->required();
    command.add_option("--path", request.selector_paths, "selector path from info --format paths");
    command.add_option("--stereo", request.stereo, "stereo export policy")
        ->check(CLI::IsMember({"none", "auto"}));
    command.add_flag("--overwrite", request.overwrite, "replace existing export files");
    command.add_flag("--strict", request.strict, "stop after the first load error");
    command.add_option("--progress", request.progress, "progress display policy")
        ->check(CLI::IsMember({"auto", "always", "never"}));
  };
  auto *extract_wav_nested =
      extract->add_subcommand("wav", "export targeted WAVs to a shared sample pool");
  configure_extract(*extract_wav_nested, extract_wav_request);
  auto *extract_sfz_nested =
      extract->add_subcommand("sfz", "export targeted WAVs and generate SFZ files");
  configure_extract(*extract_sfz_nested, extract_sfz_request);

  axk::cli::PackageExportRequest package_export_request;
  axk::cli::PackageReadRequest package_inspect_request;
  axk::cli::PackageReadRequest package_verify_request;
  axk::cli::PackageImportRequest package_plan_request;
  axk::cli::PackageImportRequest package_import_request;
  package_import_request.apply = true;
  auto *package = app.add_subcommand("package", "move self-contained sampler object graphs");
  auto *package_export = package->add_subcommand("export", "export one portable package");
  package_export->add_option("source", package_export_request.source, "source sampler image")
      ->required();
  package_export
      ->add_option("--root", package_export_request.roots,
                   "root selector: volume or program|sbac|sbnk|smpl|sequence=NAME")
      ->required()
      ->expected(1, -1);
  package_export->add_option("--partition", package_export_request.partition_index,
                             "numeric source partition index");
  package_export->add_option("--group", package_export_request.group_name,
                             "source partition or CD-ROM group label");
  package_export->add_option("--volume", package_export_request.volume_name, "source volume label");
  package_export
      ->add_option("-o,--output", package_export_request.output,
                   "package filename or suffix-free stem")
      ->required();
  package_export->add_option("--format", package_export_request.format, "summary or json")
      ->check(CLI::IsMember({"summary", "json"}));
  package_export->add_flag("--overwrite", package_export_request.overwrite,
                           "replace an existing package atomically");

  const auto configure_package_read = [](CLI::App &command, axk::cli::PackageReadRequest &request) {
    command.add_option("package", request.package, "portable package file")->required();
    command.add_option("--format", request.format, "summary or json")
        ->check(CLI::IsMember({"summary", "json"}));
  };
  auto *package_inspect = package->add_subcommand("inspect", "inspect and verify a package");
  configure_package_read(*package_inspect, package_inspect_request);
  auto *package_verify = package->add_subcommand("verify", "verify a package");
  configure_package_read(*package_verify, package_verify_request);

  const auto configure_package_import = [](CLI::App &command,
                                           axk::cli::PackageImportRequest &request) {
    command.add_option("target", request.target, "target sampler image")->required();
    command.add_option("packages", request.packages, "portable package files")
        ->required()
        ->expected(1, -1);
    command
        .add_option("--destination", request.destinations,
                    "JSON destination object with package, root, and sampler scope")
        ->required()
        ->expected(1, -1);
    command.add_option("--rename-map", request.rename_map, "JSON array of explicit node renames");
    command
        .add_option("--reuse-scope", request.reuse_scope,
                    "SFS waveform reuse scope: volume or hardware-proven-partition")
        ->check(CLI::IsMember({"volume", "hardware-proven-partition"}));
    command.add_option("--format", request.format, "summary or json")
        ->check(CLI::IsMember({"summary", "json"}));
  };
  auto *package_plan = package->add_subcommand("plan-import", "plan a package import");
  configure_package_import(*package_plan, package_plan_request);
  auto *package_import = package->add_subcommand("import", "atomically import packages");
  configure_package_import(*package_import, package_import_request);
  package_import->add_option("-o,--output", package_import_request.output, "output sampler image")
      ->required();
  package_import->add_flag("--overwrite", package_import_request.overwrite,
                           "atomically replace an existing output image");

  std::filesystem::path tree_path;
  bool tree_pretty = false;
  bool include_default_programs = false;
  auto *tree = app.add_subcommand("tree", "render sampler-facing organization as JSON");
  tree->group("");
  tree->add_option("image", tree_path, "input HDA/HDS image")->required();
  tree->add_flag("--pretty", tree_pretty, "indent JSON output");
  tree->add_flag("--include-default-programs", include_default_programs,
                 "include all 128 Program slots");

  axk::cli::OrphansRequest orphans_request;
  auto *orphans = app.add_subcommand("orphans", "classify physical waveform ownership as JSON");
  orphans->add_option("paths", orphans_request.paths, "input HDS image paths")
      ->required()
      ->expected(1, -1);
  orphans
      ->add_option("-o,--output-dir", orphans_request.output_directory,
                   "directory for orphan reports")
      ->required();
  orphans->add_flag("--overwrite", orphans_request.overwrite,
                    "allow writing into a non-empty output directory");

  axk::cli::ValidateRequest validate_request;
  auto *validate = app.add_subcommand("validate", "validate container and sampler organization");
  validate->add_option("paths", validate_request.paths, "input files or directories")
      ->expected(0, -1);
  validate->add_option("--exports", validate_request.exports, "export directory to validate");
  validate
      ->add_option("-o,--output-dir", validate_request.output_directory,
                   "directory for validation reports")
      ->required();
  validate->add_option("--policy", validate_request.policy, "validation policy")
      ->check(CLI::IsMember({"normal", "strict", "salvage-aware"}));
  validate->add_flag("--strict", validate_request.strict, "alias for --policy strict");
  validate->add_flag("--overwrite", validate_request.overwrite,
                     "allow writing into a non-empty output directory");

  std::filesystem::path extract_wav_path;
  std::filesystem::path extract_wav_output;
  bool extract_wav_overwrite = false;
  bool extract_wav_pretty = false;
  auto *extract_wav = app.add_subcommand("extract-wav", "export exact physical SMPL WAV files");
  extract_wav->group("");
  extract_wav->add_option("image", extract_wav_path, "input HDA/HDS image")->required();
  extract_wav->add_option("--output-dir", extract_wav_output, "output directory")->required();
  extract_wav->add_flag("--overwrite", extract_wav_overwrite, "replace existing WAV files");
  extract_wav->add_flag("--pretty", extract_wav_pretty, "indent JSON output");

  std::filesystem::path export_path;
  std::filesystem::path export_output;
  bool export_overwrite = false;
  bool export_sfz = false;
  bool export_pretty = false;
  auto *export_command = app.add_subcommand("export", "write structured exact audio exports");
  export_command->group("");
  export_command->add_option("image", export_path, "input HDA/HDS image")->required();
  export_command->add_option("--output-dir", export_output, "output directory")->required();
  export_command->add_flag("--overwrite", export_overwrite, "replace existing files");
  export_command->add_flag("--sfz", export_sfz, "write SFZ instruments");
  export_command->add_flag("--pretty", export_pretty, "indent command JSON output");

  std::filesystem::path preview_path;
  std::string preview_object_key;
  std::size_t preview_bins = 256;
  bool preview_pretty = false;
  auto *preview = app.add_subcommand("preview", "build a bounded waveform min/max envelope");
  preview->group("");
  preview->add_option("image", preview_path, "input HDA/HDS image")->required();
  preview->add_option("object-key", preview_object_key, "SMPL object key")->required();
  preview->add_option("--bins", preview_bins, "requested envelope bin count");
  preview->add_flag("--pretty", preview_pretty, "indent JSON output");

  std::filesystem::path create_manifest;
  std::filesystem::path create_output;
  bool create_overwrite = false;
  bool create_pretty = false;
  auto *create_hds = app.add_subcommand("create-hds", "create a fresh HDS image from a manifest");
  create_hds->group("");
  create_hds->add_option("--manifest", create_manifest, "HDS build manifest JSON")->required();
  create_hds->add_option("--output", create_output, "output HDS path")->required();
  create_hds->add_flag("--overwrite", create_overwrite, "replace an existing output");
  create_hds->add_flag("--pretty", create_pretty, "indent JSON output");
  auto *create = app.add_subcommand("create", "create a fresh sampler container");
  auto *create_hds_nested = create->add_subcommand("hds", "create a fresh HDS image");
  create_hds_nested->add_option("manifest", create_manifest, "HDS build manifest JSON")->required();
  create_hds_nested->add_option("-o,--output", create_output, "output HDS path")->required();
  create_hds_nested->add_flag("--overwrite", create_overwrite, "replace an existing output");
  create_hds_nested->add_flag("--pretty", create_pretty, "indent JSON output");
  auto *create_floppy = create->add_subcommand("floppy", "create a fresh FAT12 floppy image");
  create_floppy->add_option("manifest", create_manifest, "media build manifest JSON")->required();
  create_floppy->add_option("-o,--output", create_output, "output IMA path")->required();
  create_floppy->add_flag("--overwrite", create_overwrite, "replace an existing output");
  create_floppy->add_flag("--pretty", create_pretty, "indent JSON output");
  auto *create_iso = create->add_subcommand("iso", "create a fresh ISO9660 image");
  create_iso->add_option("manifest", create_manifest, "media build manifest JSON")->required();
  create_iso->add_option("-o,--output", create_output, "output ISO path")->required();
  create_iso->add_flag("--overwrite", create_overwrite, "replace an existing output");
  create_iso->add_flag("--pretty", create_pretty, "indent JSON output");

  std::filesystem::path alter_source;
  std::filesystem::path alter_manifest;
  std::filesystem::path alter_output;
  bool alter_pretty = false;
  auto *alter = app.add_subcommand("alter", "alter an existing sampler container");
  auto *alter_hds = alter->add_subcommand("hds", "plan or apply an HDS transaction");
  alter_hds->add_option("source", alter_source, "source HDS image")->required();
  alter_hds->add_option("manifest", alter_manifest, "alteration manifest JSON")->required();
  alter_hds->add_option("-o,--output", alter_output, "new output HDS path");
  alter_hds->add_flag("--pretty", alter_pretty, "indent JSON output");

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
  if (*object_json)
    return run_objects(object_json_path, object_json_pretty);
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
  if (*tree)
    return run_tree(tree_path, tree_pretty, include_default_programs);
  if (*orphans)
    return run_orphans_request(orphans_request);
  if (*validate) {
    if (validate_request.strict)
      validate_request.policy = "strict";
    return run_validate_request(validate_request);
  }
  if (*extract_wav) {
    return run_extract_wav(extract_wav_path, extract_wav_output, extract_wav_overwrite,
                           extract_wav_pretty);
  }
  if (*export_command) {
    return run_export(export_path, export_output, export_overwrite, export_sfz, export_pretty);
  }
  if (*preview)
    return run_preview(preview_path, preview_object_key, preview_bins, preview_pretty);
  if (*create_hds || *create_hds_nested) {
    return run_create_hds(create_manifest, create_output, create_overwrite, create_pretty);
  }
  if (*create_floppy) {
    return run_create_media(create_manifest, create_output, "fat12_floppy", create_overwrite,
                            create_pretty);
  }
  if (*create_iso) {
    return run_create_media(create_manifest, create_output, "iso9660", create_overwrite,
                            create_pretty);
  }
  if (*alter_hds) {
    const auto output =
        !alter_output.empty() ? std::optional<std::filesystem::path>{alter_output} : std::nullopt;
    return run_alter_hds(alter_source, alter_manifest, output, alter_pretty);
  }
  std::cout << app.help();
  return 0;
}
