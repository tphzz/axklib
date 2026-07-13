#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>

#include "content_id.hpp"
#include "handlers.hpp"
#include "requests.hpp"
#include "schema/export_v1.hpp"
#include "schema/objects_v1.hpp"
#include "schema/operations_v1.hpp"
#include "schema/semantic_v1.hpp"
#include "support.hpp"

#include "axklib/alteration.hpp"
#include "axklib/audio.hpp"
#include "axklib/audio_export.hpp"
#include "axklib/catalog.hpp"
#include "axklib/error.hpp"
#include "axklib/media.hpp"
#include "axklib/object.hpp"
#include "axklib/relationship.hpp"
#include "axklib/report.hpp"
#include "axklib/semantic.hpp"
#include "axklib/sfs.hpp"
#include "axklib/utf8.hpp"
#include "axklib/version.hpp"
#include "axklib/writer.hpp"

namespace axk::cli::commands {

int run_objects(const std::filesystem::path &path, bool pretty) {
  const auto media = axk::open_media(path);
  if (!media) {
    std::cerr << axk::render_error(media.error()) << '\n';
    return 2;
  }
  if (media->kind() != axk::MediaKind::sfs) {
    const auto decoded_objects = media->objects();
    if (!decoded_objects) {
      std::cerr << axk::render_error(decoded_objects.error()) << '\n';
      return 2;
    }
    const auto status_name = [](axk::LabelStatus status) {
      switch (status) {
      case axk::LabelStatus::confirmed:
        return "confirmed";
      case axk::LabelStatus::navigation_aid:
        return "navigation-aid";
      case axk::LabelStatus::raw_identifier:
        return "raw-identifier";
      }
      return "raw-identifier";
    };
    const auto kind_name = [](axk::MediaKind kind) {
      switch (kind) {
      case axk::MediaKind::fat12_floppy:
        return "fat12_floppy";
      case axk::MediaKind::iso9660:
        return "iso9660";
      case axk::MediaKind::standalone_object:
        return "standalone_object";
      case axk::MediaKind::sfs:
        return "sfs";
      }
      return "unknown";
    };
    axk::cli::schema::objects_v1::ObjectsOutput output{
        .shape = axk::cli::schema::objects_v1::ContainerShape::media,
        .container_kind = kind_name(media->kind()),
        .objects = {},
    };
    for (const auto &object : *decoded_objects) {
      const auto structured = axk::structured_object_path(object);
      output.objects.push_back({
          .partition_index = std::nullopt,
          .sfs_id = std::nullopt,
          .key = object.key,
          .logical_path = object.logical_path,
          .scope_key = object.scope_key,
          .raw_group = object.raw_group,
          .raw_volume = object.raw_volume,
          .group_label = object.group_label.value,
          .group_label_status = status_name(object.group_label.status),
          .group_label_basis = object.group_label.basis,
          .volume_label = object.volume_label.value,
          .volume_label_status = status_name(object.volume_label.status),
          .volume_label_basis = object.volume_label.basis,
          .data_offset = object.data_offset,
          .size = object.size,
          .structured_path_utf8 = axk::text::path_to_utf8(structured.relative_path),
          .header = object.decoded.header,
          .decoded = object.decoded,
      });
    }
    const auto serialized = axk::cli::schema::objects_v1::serialize(output, pretty);
    if (!serialized)
      return report_failure(serialized.error());
    std::cout << *serialized << '\n';
    return 0;
  }
  const auto container = axk::open_image(path);
  if (!container) {
    std::cerr << axk::render_error(container.error()) << '\n';
    return 2;
  }
  axk::cli::schema::objects_v1::ObjectsOutput output{
      .shape = axk::cli::schema::objects_v1::ContainerShape::sfs,
      .container_kind = {},
      .objects = {},
  };
  for (const auto &partition : container->partitions()) {
    for (const auto &record : partition.records) {
      if (record.payload_kind != axk::PayloadKind::object)
        continue;
      const auto payload =
          container->read_record_data(partition.index, record.sfs_id, 64U * 1024U * 1024U);
      if (!payload) {
        std::cerr << axk::render_error(payload.error()) << '\n';
        return 2;
      }
      const auto decoded = axk::decode_object(*payload);
      if (!decoded) {
        std::cerr << axk::render_error(decoded.error()) << '\n';
        return 2;
      }
      output.objects.push_back({
          .partition_index = partition.index.value,
          .sfs_id = record.sfs_id.value,
          .key = {},
          .logical_path = {},
          .scope_key = {},
          .raw_group = {},
          .raw_volume = {},
          .group_label = {},
          .group_label_status = {},
          .group_label_basis = {},
          .volume_label = {},
          .volume_label_status = {},
          .volume_label_basis = {},
          .data_offset = 0U,
          .size = 0U,
          .structured_path_utf8 = {},
          .header = decoded->header,
          .decoded = std::move(*decoded),
      });
    }
  }
  const auto serialized = axk::cli::schema::objects_v1::serialize(output, pretty);
  if (!serialized)
    return report_failure(serialized.error());
  std::cout << *serialized << '\n';
  return 0;
}

axk::Result<SemanticSnapshot> load_semantic_snapshot(const std::filesystem::path &path) {
  auto container = axk::open_image(path);
  if (!container)
    return std::unexpected{container.error()};
  auto catalog = axk::build_object_catalog(*container);
  if (!catalog)
    return std::unexpected{catalog.error()};
  auto graph = axk::build_relationship_graph(*catalog);
  return SemanticSnapshot{std::move(*container), std::move(*catalog), std::move(graph)};
}

int run_relationships(const std::filesystem::path &path, bool pretty) {
  const auto media = axk::open_media(path);
  if (!media) {
    std::cerr << axk::render_error(media.error()) << '\n';
    return 2;
  }
  if (media->kind() != axk::MediaKind::sfs) {
    const auto catalog = axk::build_object_catalog(*media);
    if (!catalog) {
      std::cerr << axk::render_error(catalog.error()) << '\n';
      return 2;
    }
    const auto graph = axk::build_relationship_graph(*catalog);
    const auto serialized = axk::cli::schema::semantic_v1::serialize(
        axk::cli::schema::semantic_v1::project_relationships(graph), pretty);
    if (!serialized)
      return report_failure(serialized.error());
    std::cout << *serialized << '\n';
    return 0;
  }
  const auto snapshot = load_semantic_snapshot(path);
  if (!snapshot) {
    std::cerr << axk::render_error(snapshot.error()) << '\n';
    return 2;
  }
  const auto serialized = axk::cli::schema::semantic_v1::serialize(
      axk::cli::schema::semantic_v1::project_relationships(snapshot->graph), pretty);
  if (!serialized)
    return report_failure(serialized.error());
  std::cout << *serialized << '\n';
  return 0;
}

int run_tree(const std::filesystem::path &path, bool pretty, bool include_default_programs) {
  const auto media = axk::open_media(path);
  if (!media) {
    std::cerr << axk::render_error(media.error()) << '\n';
    return 2;
  }
  if (media->kind() != axk::MediaKind::sfs) {
    const auto catalog = axk::build_object_catalog(*media);
    if (!catalog) {
      std::cerr << axk::render_error(catalog.error()) << '\n';
      return 2;
    }
    const auto graph = axk::build_relationship_graph(*catalog);
    const auto tree = axk::build_content_tree(axk::text::path_to_utf8(path), *catalog, graph,
                                              include_default_programs);
    const auto serialized = axk::cli::schema::semantic_v1::serialize(
        axk::cli::schema::semantic_v1::project_tree(tree), pretty);
    if (!serialized)
      return report_failure(serialized.error());
    std::cout << *serialized << '\n';
    return 0;
  }
  const auto snapshot = load_semantic_snapshot(path);
  if (!snapshot) {
    std::cerr << axk::render_error(snapshot.error()) << '\n';
    return 2;
  }
  const auto tree = axk::build_content_tree(snapshot->container, snapshot->catalog, snapshot->graph,
                                            include_default_programs);
  const auto serialized = axk::cli::schema::semantic_v1::serialize(
      axk::cli::schema::semantic_v1::project_tree(tree), pretty);
  if (!serialized)
    return report_failure(serialized.error());
  std::cout << *serialized << '\n';
  return 0;
}

int run_extract_wav(const std::filesystem::path &path, const std::filesystem::path &output_dir,
                    bool overwrite, bool pretty) {
  const auto snapshot = load_semantic_snapshot(path);
  if (!snapshot) {
    std::cerr << axk::render_error(snapshot.error()) << '\n';
    return 2;
  }
  std::vector<axk::cli::schema::semantic_v1::WaveformOutput> rows;
  for (const auto &item : snapshot->catalog.objects) {
    if (item.object.header.type != axk::ObjectType::smpl)
      continue;
    const auto waveform = axk::decode_waveform(snapshot->container, item);
    if (!waveform) {
      std::cerr << axk::render_error(waveform.error()) << '\n';
      return 2;
    }
    const auto filename = std::format("p{}_sfs{}.wav", item.partition.value, item.sfs_id.value);
    const auto output = output_dir / filename;
    if (const auto written = axk::write_wav_atomic(output, *waveform, overwrite); !written) {
      std::cerr << axk::render_error(written.error()) << '\n';
      return 2;
    }
    rows.push_back({
        .partition_index = item.partition.value,
        .sfs_id = item.sfs_id.value,
        .object_key = item.key,
        .name = waveform->name,
        .wav_path_utf8 = axk::text::path_to_utf8(output),
        .sample_rate = waveform->format.sample_rate,
        .sample_width_bytes = waveform->format.sample_width_bytes,
        .stored_sample_width_bytes = waveform->stored_sample_width_bytes,
        .frame_count = waveform->frame_count,
        .stored_payload_size = waveform->stored_payload_size,
        .stored_payload_transform = waveform->stored_payload_transform,
        .alternating_byte_payload_detected = waveform->alternating_byte_payload_detected,
    });
  }
  const auto serialized = axk::cli::schema::semantic_v1::serialize(rows, pretty);
  if (!serialized)
    return report_failure(serialized.error());
  std::cout << *serialized << '\n';
  return 0;
}

int run_export(const std::filesystem::path &path, const std::filesystem::path &output_dir,
               bool overwrite, bool sfz, bool pretty) {
  const auto snapshot = load_semantic_snapshot(path);
  if (!snapshot) {
    std::cerr << axk::render_error(snapshot.error()) << '\n';
    return 2;
  }
  const auto plan = axk::build_export_plan(snapshot->container, snapshot->catalog, snapshot->graph);
  if (!plan) {
    std::cerr << axk::render_error(plan.error()) << '\n';
    return 2;
  }
  if (!overwrite) {
    for (const auto &volume : plan->volumes) {
      const auto graph_path = output_dir / volume.relative_root / "volume.axklib.json";
      if (std::filesystem::exists(graph_path)) {
        std::cerr << "refusing to replace existing graph " << graph_path << '\n';
        return 2;
      }
    }
  }
  const auto written = axk::write_export_audio(*plan, output_dir, overwrite);
  if (!written) {
    std::cerr << axk::render_error(written.error()) << '\n';
    return 2;
  }
  if (sfz) {
    const auto sfz_result = axk::write_sfz(*plan, output_dir, overwrite);
    if (!sfz_result) {
      std::cerr << axk::render_error(sfz_result.error()) << '\n';
      return 2;
    }
  }
  std::vector<axk::cli::schema::export_v1::VolumeSummaryOutput> volumes;
  for (const auto &volume : plan->volumes) {
    auto value = axk::cli::schema::export_v1::serialize_volume_graph(volume, snapshot->graph, path);
    if (!value)
      return report_failure(value.error());
    const auto graph_path = output_dir / volume.relative_root / "volume.axklib.json";
    std::error_code error;
    std::filesystem::create_directories(graph_path.parent_path(), error);
    auto temporary_name = std::filesystem::path{"."};
    temporary_name += graph_path.filename().native();
    temporary_name += ".tmp";
    const auto temporary = graph_path.parent_path() / temporary_name;
    {
      std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
      output << *value << '\n';
      if (!output)
        error = std::make_error_code(std::errc::io_error);
    }
    if (!error && overwrite)
      std::filesystem::remove(graph_path, error);
    if (!error)
      std::filesystem::rename(temporary, graph_path, error);
    if (error) {
      std::filesystem::remove(temporary, error);
      std::cerr << "could not write " << graph_path << '\n';
      return 2;
    }
    volumes.push_back({
        .path_utf8 = axk::text::path_to_utf8(volume.relative_root),
        .graph_path_utf8 = axk::text::path_to_utf8(graph_path),
        .waveform_count = volume.waveforms.size(),
        .sample_bank_count = volume.sample_banks.size(),
    });
  }
  const auto serialized = axk::cli::schema::export_v1::serialize(volumes, pretty);
  if (!serialized)
    return report_failure(serialized.error());
  std::cout << *serialized << '\n';
  return 0;
}

int run_preview(const std::filesystem::path &path, std::string_view object_key, std::size_t bins,
                bool pretty) {
  const auto snapshot = load_semantic_snapshot(path);
  if (!snapshot) {
    std::cerr << axk::render_error(snapshot.error()) << '\n';
    return 2;
  }
  const auto found =
      std::ranges::find(snapshot->catalog.objects, object_key, &axk::ObjectSnapshot::key);
  if (found == snapshot->catalog.objects.end()) {
    std::cerr << "waveform is not part of this image\n";
    return 2;
  }
  const auto waveform = axk::decode_waveform(snapshot->container, *found);
  if (!waveform) {
    std::cerr << axk::render_error(waveform.error()) << '\n';
    return 2;
  }
  const auto preview = axk::build_preview_envelope(*waveform, bins);
  if (!preview) {
    std::cerr << axk::render_error(preview.error()) << '\n';
    return 2;
  }
  const auto serialized = axk::cli::schema::operations_v1::serialize(
      axk::cli::schema::operations_v1::project_preview(object_key, *preview), pretty);
  if (!serialized) {
    std::cerr << axk::render_error(serialized.error()) << '\n';
    return 2;
  }
  std::cout << *serialized << '\n';
  return 0;
}

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
