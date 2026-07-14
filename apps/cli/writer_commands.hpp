#pragma once

#include <filesystem>
#include <string>

namespace CLI {
class App;
}

namespace axk::cli {

struct WriterCommandState {
  std::filesystem::path create_manifest;
  std::filesystem::path create_output;
  std::string create_manifest_kind;
  std::filesystem::path create_manifest_output;
  bool create_overwrite{};
  bool create_pretty{};
  bool create_manifest_overwrite{};
  CLI::App *create_hds_legacy{};
  CLI::App *create_hds{};
  CLI::App *create_floppy{};
  CLI::App *create_iso{};
  CLI::App *create_manifest_command{};

  std::filesystem::path alter_source;
  std::filesystem::path alter_manifest;
  std::filesystem::path alter_output;
  std::filesystem::path alter_manifest_output;
  bool alter_pretty{};
  bool alter_manifest_overwrite{};
  CLI::App *alter_hds{};
  CLI::App *alter_manifest_command{};
};

void register_writer_commands(CLI::App &app, WriterCommandState &state);

} // namespace axk::cli
