#include "handlers.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>

#include "schema/operations_v1.hpp"
#include "support.hpp"

#include "axklib/alteration.hpp"
#include "axklib/catalog.hpp"
#include "axklib/media.hpp"
#include "axklib/utf8.hpp"
#include "axklib/writer.hpp"

namespace axk::cli::commands {

int run_create_hds(const std::filesystem::path &manifest_path,
                   const std::filesystem::path &output_path, bool overwrite, bool pretty) {
  static_cast<void>(pretty);
  const auto manifest = axk::load_hds_build_manifest(manifest_path);
  if (!manifest) {
    std::cerr << axk::render_error(manifest.error()) << '\n';
    return 2;
  }
  const auto written = axk::write_hds_image(*manifest, output_path, overwrite);
  if (!written) {
    std::cerr << axk::render_error(written.error()) << '\n';
    return 2;
  }
  std::size_t object_count{};
  if (const auto media = axk::open_media(written->path); media) {
    if (const auto catalog = axk::build_object_catalog(*media); catalog)
      object_count = catalog->objects.size();
  }
  std::cout << "image=" << axk::text::path_to_utf8(written->path)
            << " size_bytes=" << written->size_bytes << " partitions=" << written->partitions.size()
            << " objects=" << object_count
            << " unused_tail_sectors=" << written->unused_tail_sectors << '\n';
  for (const auto &partition : written->partitions) {
    std::cout << "partition=" << static_cast<unsigned int>(partition.geometry.index) << " name='"
              << partition.name << "' start_sector=" << partition.geometry.start_sector
              << " sector_count=" << partition.geometry.filesystem_sector_count
              << " cluster_count=" << partition.geometry.cluster_count
              << " free_kib=" << partition.sampler_visible_free_kib << '\n';
  }
  return 0;
}

int run_create_media(const std::filesystem::path &manifest_path,
                     const std::filesystem::path &output_path, std::string_view expected_format,
                     bool overwrite, bool pretty) {
  static_cast<void>(pretty);
  const auto manifest = axk::load_media_build_manifest(manifest_path);
  if (!manifest) {
    std::cerr << axk::render_error(manifest.error()) << '\n';
    return 2;
  }
  const auto actual_format = manifest->format == axk::MediaImageFormat::fat12_floppy
                                 ? std::string_view{"fat12_floppy"}
                                 : std::string_view{"iso9660"};
  if (actual_format != expected_format) {
    std::cerr << "manifest format '" << actual_format << "' does not match create "
              << (expected_format == "fat12_floppy" ? "floppy" : "iso") << '\n';
    return 2;
  }
  const auto written = axk::write_media_image(*manifest, output_path, overwrite);
  if (!written) {
    std::cerr << axk::render_error(written.error()) << '\n';
    return 2;
  }
  std::cout << "image=" << axk::text::path_to_utf8(written->path) << " format=" << actual_format
            << " size_bytes=" << written->size_bytes << " objects=" << written->object_count
            << '\n';
  return 0;
}

int run_create_manifest(std::string_view kind, const std::filesystem::path &output_path,
                        bool overwrite) {
  std::optional<axk::BuildManifestKind> manifest_kind;
  if (kind == "hds")
    manifest_kind = axk::BuildManifestKind::hds;
  else if (kind == "floppy")
    manifest_kind = axk::BuildManifestKind::fat12_floppy;
  else if (kind == "iso")
    manifest_kind = axk::BuildManifestKind::iso9660;
  if (!manifest_kind) {
    std::cerr << "manifest kind must be hds, floppy, or iso\n";
    return 2;
  }
  const auto written = axk::write_build_manifest_template(*manifest_kind, output_path, overwrite);
  if (!written) {
    std::cerr << axk::render_error(written.error()) << '\n';
    return 2;
  }
  std::cout << "manifest=" << axk::text::path_to_utf8(output_path) << " kind=" << kind << '\n';
  return 0;
}

int run_alter_manifest(const std::filesystem::path &output_path, bool overwrite) {
  const auto written = axk::write_alteration_manifest_template(output_path, overwrite);
  if (!written) {
    std::cerr << axk::render_error(written.error()) << '\n';
    return 2;
  }
  std::cout << "manifest=" << axk::text::path_to_utf8(output_path) << " kind=alteration\n";
  return 0;
}

int run_alter_hds(const std::filesystem::path &source_path,
                  const std::filesystem::path &manifest_path,
                  const std::optional<std::filesystem::path> &output_path, bool pretty) {
  const auto manifest = axk::load_alteration_manifest(manifest_path);
  if (!manifest) {
    std::cerr << axk::render_error(manifest.error()) << '\n';
    return 2;
  }
  const auto altered = axk::alter_hds(source_path, *manifest, output_path);
  if (!altered) {
    std::cerr << axk::render_error(altered.error()) << '\n';
    return 2;
  }
  const auto serialized = axk::cli::schema::operations_v1::serialize(
      axk::cli::schema::operations_v1::project_alteration(*altered), pretty);
  if (!serialized) {
    std::cerr << axk::render_error(serialized.error()) << '\n';
    return 2;
  }
  std::cout << *serialized << '\n';
  return 0;
}

} // namespace axk::cli::commands
