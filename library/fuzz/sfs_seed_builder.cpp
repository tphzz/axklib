#include <filesystem>
#include <iostream>
#include <utility>

#include "axklib/error.hpp"
#include "axklib/writer.hpp"

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: axk_sfs_seed_builder OUTPUT.hds\n";
    return 2;
  }

  axk::VolumeSpec volume;
  volume.name = "Fuzz Volume";
  axk::HdsBuildManifest manifest{"1.0", axk::minimum_hds_size, {}};
  manifest.partitions.push_back({"hd1", {std::move(volume)}});
  const auto written = axk::write_hds_image(manifest, std::filesystem::path{argv[1]}, true);
  if (!written) {
    std::cerr << axk::render_error(written.error()) << '\n';
    return 1;
  }
  return 0;
}
