#include "writer_commands.hpp"

#include <CLI/CLI.hpp>

void axk::cli::register_writer_commands(CLI::App &app, WriterCommandState &state) {
  auto *create = app.add_subcommand("create", "create a fresh sampler container");
  state.create_hds = create->add_subcommand("hds", "create a fresh HDS image");
  state.create_hds->add_option("manifest", state.create_manifest, "HDS build manifest JSON")
      ->required();
  state.create_hds->add_option("-o,--output", state.create_output, "output HDS path")->required();
  state.create_hds->add_flag("--overwrite", state.create_overwrite, "replace an existing output");
  state.create_hds->add_flag("--pretty", state.create_pretty, "indent JSON output");

  state.create_floppy = create->add_subcommand("floppy", "create a fresh FAT12 floppy image");
  state.create_floppy->add_option("manifest", state.create_manifest, "media build manifest JSON")
      ->required();
  state.create_floppy->add_option("-o,--output", state.create_output, "output IMA path")
      ->required();
  state.create_floppy->add_flag("--overwrite", state.create_overwrite,
                                "replace an existing output");
  state.create_floppy->add_flag("--pretty", state.create_pretty, "indent JSON output");

  state.create_iso = create->add_subcommand("iso", "create a fresh ISO9660 image");
  state.create_iso->add_option("manifest", state.create_manifest, "media build manifest JSON")
      ->required();
  state.create_iso->add_option("-o,--output", state.create_output, "output ISO path")->required();
  state.create_iso->add_flag("--overwrite", state.create_overwrite, "replace an existing output");
  state.create_iso->add_flag("--pretty", state.create_pretty, "indent JSON output");

  state.create_manifest_command =
      create->add_subcommand("manifest", "write a starter build manifest");
  state.create_manifest_command
      ->add_option("kind", state.create_manifest_kind, "hds, floppy, or iso")
      ->required()
      ->check(CLI::IsMember({"hds", "floppy", "iso"}));
  state.create_manifest_command
      ->add_option("-o,--output", state.create_manifest_output, "output JSON path")
      ->required();
  state.create_manifest_command->add_flag("--overwrite", state.create_manifest_overwrite,
                                          "replace an existing manifest");

  auto *alter = app.add_subcommand("alter", "alter an existing sampler container");
  state.alter_hds = alter->add_subcommand("hds", "plan or apply an HDS transaction");
  state.alter_hds->add_option("source", state.alter_source, "source HDS image")->required();
  state.alter_hds->add_option("manifest", state.alter_manifest, "alteration manifest JSON")
      ->required();
  state.alter_hds->add_option("-o,--output", state.alter_output, "new output HDS path");
  state.alter_hds->add_flag("--pretty", state.alter_pretty, "indent JSON output");

  state.alter_manifest_command =
      alter->add_subcommand("manifest", "write a starter alteration manifest");
  state.alter_manifest_command
      ->add_option("-o,--output", state.alter_manifest_output, "output JSON path")
      ->required();
  state.alter_manifest_command->add_flag("--overwrite", state.alter_manifest_overwrite,
                                         "replace an existing manifest");
}
