#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

#include <sys/resource.h>
#include <unistd.h>

#include "axklib/catalog.hpp"
#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

int main() {
  constexpr std::uint64_t memory_budget_kib = 256U * 1024U;
  axk::HdsBuildManifest manifest{"1.0", axk::maximum_hds_size, {}};
  for (std::uint8_t index = 0; index < 8U; ++index) {
    axk::VolumeSpec volume;
    volume.name = "Volume " + std::to_string(index + 1U);
    manifest.partitions.push_back({"hd" + std::to_string(index + 1U), {std::move(volume)}});
  }

  const auto path = std::filesystem::temp_directory_path() /
                    ("axklib-large-memory-" + std::to_string(::getpid()) + ".hds");
  std::error_code error;
  std::filesystem::remove(path, error);
  const auto written = axk::write_hds_image(manifest, path);
  if (!written) {
    std::cerr << axk::render_error(written.error()) << '\n';
    return 1;
  }
  const auto image = axk::open_image(path);
  if (!image) {
    std::cerr << axk::render_error(image.error()) << '\n';
    return 2;
  }
  const auto catalog = axk::build_object_catalog(*image);
  if (!catalog) {
    std::cerr << axk::render_error(catalog.error()) << '\n';
    return 3;
  }

  struct rusage usage{};
  const auto usage_ok = ::getrusage(RUSAGE_SELF, &usage) == 0;
  std::filesystem::remove(path, error);
  if (!usage_ok || static_cast<std::uint64_t>(usage.ru_maxrss) > memory_budget_kib) {
    std::cerr << "large-image peak memory exceeded " << memory_budget_kib
              << " KiB: " << usage.ru_maxrss << " KiB\n";
    return 4;
  }
  std::cout << "2 GiB / 8-partition inventory peak: " << usage.ru_maxrss << " KiB\n";
  return 0;
}
