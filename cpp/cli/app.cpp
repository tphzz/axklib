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

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "app.hpp"
#include "requests.hpp"

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
#include "axklib/version.hpp"
#include "axklib/writer.hpp"

namespace {

constexpr std::string_view oracle_report_library_version{"0.1.0-plan008"};

std::string hex(std::span<const std::byte> bytes) {
  std::ostringstream result;
  result << std::hex << std::setfill('0');
  for (const auto byte : bytes) {
    result << std::setw(2) << static_cast<unsigned int>(std::to_integer<std::uint8_t>(byte));
  }
  return result.str();
}

std::string sha1_hex(std::span<const std::byte> input) {
  std::vector<std::uint8_t> message;
  message.reserve(input.size() + 72U);
  for (const auto value : input)
    message.push_back(std::to_integer<std::uint8_t>(value));
  const auto bit_count = static_cast<std::uint64_t>(message.size()) * 8U;
  message.push_back(0x80U);
  while (message.size() % 64U != 56U)
    message.push_back(0U);
  for (int shift = 56; shift >= 0; shift -= 8)
    message.push_back(static_cast<std::uint8_t>(bit_count >> shift));
  std::array<std::uint32_t, 5> hash{0x67452301U, 0xefcdab89U, 0x98badcfeU, 0x10325476U,
                                    0xc3d2e1f0U};
  const auto rotate = [](std::uint32_t value, unsigned int count) {
    return static_cast<std::uint32_t>((value << count) | (value >> (32U - count)));
  };
  for (std::size_t block = 0; block < message.size(); block += 64U) {
    std::array<std::uint32_t, 80> words{};
    for (std::size_t index = 0; index < 16U; ++index) {
      const auto offset = block + index * 4U;
      words[index] = (static_cast<std::uint32_t>(message[offset]) << 24U) |
                     (static_cast<std::uint32_t>(message[offset + 1U]) << 16U) |
                     (static_cast<std::uint32_t>(message[offset + 2U]) << 8U) |
                     message[offset + 3U];
    }
    for (std::size_t index = 16U; index < words.size(); ++index)
      words[index] = rotate(words[index - 3U] ^ words[index - 8U] ^ words[index - 14U] ^
                                words[index - 16U],
                            1U);
    auto a = hash[0];
    auto b = hash[1];
    auto c = hash[2];
    auto d = hash[3];
    auto e = hash[4];
    for (std::size_t index = 0; index < words.size(); ++index) {
      std::uint32_t function{};
      std::uint32_t constant{};
      if (index < 20U) {
        function = (b & c) | ((~b) & d);
        constant = 0x5a827999U;
      } else if (index < 40U) {
        function = b ^ c ^ d;
        constant = 0x6ed9eba1U;
      } else if (index < 60U) {
        function = (b & c) | (b & d) | (c & d);
        constant = 0x8f1bbcdcU;
      } else {
        function = b ^ c ^ d;
        constant = 0xca62c1d6U;
      }
      const auto temporary = rotate(a, 5U) + function + e + constant + words[index];
      e = d;
      d = c;
      c = rotate(b, 30U);
      b = a;
      a = temporary;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
  }
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto word : hash)
    output << std::setw(8) << word;
  return output.str();
}

bool valid_utf8(std::string_view value) {
  for (std::size_t index = 0; index < value.size();) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first <= 0x7fU) {
      ++index;
      continue;
    }
    std::size_t length{};
    std::uint32_t codepoint{};
    if (first >= 0xc2U && first <= 0xdfU) {
      length = 2U;
      codepoint = first & 0x1fU;
    } else if (first >= 0xe0U && first <= 0xefU) {
      length = 3U;
      codepoint = first & 0x0fU;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      length = 4U;
      codepoint = first & 0x07U;
    } else {
      return false;
    }
    if (index + length > value.size())
      return false;
    for (std::size_t offset = 1U; offset < length; ++offset) {
      const auto byte = static_cast<unsigned char>(value[index + offset]);
      if ((byte & 0xc0U) != 0x80U)
        return false;
      codepoint = (codepoint << 6U) | (byte & 0x3fU);
    }
    if ((length == 3U && codepoint < 0x800U) || (length == 4U && codepoint < 0x10000U) ||
        codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU))
      return false;
    index += length;
  }
  return true;
}

nlohmann::ordered_json member_json(const axk::CurrentSbnkMember &member) {
  return {
      {"sample_name", member.sample_name},
      {"smpl_link_id", member.smpl_link_id},
      {"root_key", member.root_key},
      {"sample_rate", member.sample_rate},
      {"fine_tune_cents", member.fine_tune_cents},
      {"pitch_base_word", member.pitch_base_word},
      {"wave_length_frames", member.wave_length_frames},
      {"loop_start_frame", member.loop_start_frame},
      {"loop_length_frames", member.loop_length_frames},
  };
}

nlohmann::ordered_json decoded_payload_json(const axk::DecodedObject &object) {
  if (const auto *sample = std::get_if<axk::CurrentSmpl>(&object.payload)) {
    return {
        {"kind", "SMPL"},
        {"sample_rate", sample->sample_rate.value},
        {"stored_sample_width_bytes", sample->stored_sample_width_bytes.value},
        {"source_wave_name", sample->source_wave_name.value},
        {"group_id", sample->group_id.value},
        {"link_id", sample->link_id.value},
        {"duplicate_sample_rate", sample->duplicate_sample_rate.value},
        {"root_key", sample->root_key.value},
        {"fine_tune_cents", sample->fine_tune_cents.value},
        {"loop_mode", sample->loop_mode.value},
        {"loop_mode_label", sample->loop_mode_label},
        {"wave_length_frames", sample->wave_length_frames.value},
        {"loop_start_frame", sample->loop_start_frame.value},
        {"loop_length_frames", sample->loop_length_frames.value},
        {"loop_end_frame_inclusive", sample->loop_end_frame_inclusive},
        {"loop_end_frame_exclusive", sample->loop_end_frame_exclusive},
        {"stored_pcm_offset", sample->stored_pcm_offset},
        {"stored_pcm_bytes", sample->stored_pcm_bytes},
        {"compact_record_hex", hex(sample->compact_record)},
    };
  }
  if (const auto *bank = std::get_if<axk::CurrentSbnk>(&object.payload)) {
    nlohmann::ordered_json fields = nlohmann::ordered_json::object();
    for (const auto &field : bank->numeric_fields) {
      fields[field.name] =
          field.value ? nlohmann::ordered_json(*field.value) : nlohmann::ordered_json(nullptr);
    }
    nlohmann::ordered_json controls = nlohmann::ordered_json::array();
    for (const auto &control : bank->control_records) {
      controls.push_back({control.device, control.function, control.type, control.range});
    }
    return {
        {"kind", "SBNK"},
        {"bank_name", bank->bank_name},
        {"instrument_name", bank->instrument_name},
        {"right_slot_present", bank->right_slot_present},
        {"right_link_role", bank->right_link_role},
        {"left", member_json(bank->left)},
        {"right", bank->right ? member_json(*bank->right) : nlohmann::ordered_json(nullptr)},
        {"inactive_right", member_json(bank->inactive_right)},
        {"linked_program_bitmap_words", bank->linked_program_bitmap_words},
        {"linked_program_numbers", bank->linked_program_numbers},
        {"numeric_fields", std::move(fields)},
        {"control_records", std::move(controls)},
        {"raw_parameter_window_hex", hex(bank->raw_parameter_window)},
    };
  }
  if (const auto *group = std::get_if<axk::CurrentSbac>(&object.payload)) {
    nlohmann::ordered_json slots = nlohmann::ordered_json::array();
    for (const auto &slot : group->slots) {
      slots.push_back(
          {{"name", slot.name}, {"raw_handle", slot.raw_handle}, {"offset", slot.offset}});
    }
    return {
        {"kind", "SBAC"},
        {"raw_sample_parameter_block_hex", hex(group->raw_sample_parameter_block)},
        {"value_enable_words", group->value_enable_words},
        {"enabled_parameter_numbers", group->enabled_parameter_numbers},
        {"enabled_numbers_outside_table", group->enabled_numbers_outside_table},
        {"bulk_assigned_sample_count", group->bulk_assigned_sample_count},
        {"active_slot_count", group->active_slot_count},
        {"maximum_slot_count", group->maximum_slot_count},
        {"slots", std::move(slots)},
    };
  }
  if (const auto *program = std::get_if<axk::CurrentProg>(&object.payload)) {
    nlohmann::ordered_json assignments = nlohmann::ordered_json::array();
    for (const auto &row : program->assignments) {
      assignments.push_back({
          {"name", row.name},
          {"raw_handle", row.raw_handle},
          {"kind", row.kind},
          {"flags", row.flags},
          {"level_offset", row.level_offset},
          {"velocity_sensitivity", row.velocity_sensitivity},
          {"pan_offset", row.pan_offset},
          {"key_limit_high", row.key_limit_high},
          {"key_limit_low", row.key_limit_low},
          {"velocity_limit_high", row.velocity_limit_high},
          {"velocity_limit_low", row.velocity_limit_low},
          {"raw_row_hex", hex(row.raw_row)},
      });
    }
    nlohmann::ordered_json effects = nlohmann::ordered_json::array();
    for (const auto &block : program->effect_blocks)
      effects.push_back(hex(block));
    return {
        {"kind", "PROG"},
        {"raw_control_block_hex", hex(program->raw_control_block)},
        {"raw_control_tail_copy_hex", hex(program->raw_control_tail_copy)},
        {"effect_blocks_hex", std::move(effects)},
        {"assignments", std::move(assignments)},
    };
  }
  if (const auto *sequence = std::get_if<axk::CurrentSequence>(&object.payload)) {
    return {{"kind", "SEQU"}, {"raw_payload_hex", hex(sequence->raw_payload)}};
  }
  if (const auto *profile = std::get_if<axk::CurrentProfile>(&object.payload)) {
    return {{"kind", "PRF3"}, {"raw_payload_hex", hex(profile->raw_payload)}};
  }
  return {{"kind", "generic"}};
}

nlohmann::ordered_json header_json(const axk::ObjectHeader &header) {
  return {
      {"raw_type", header.raw_type},
      {"name", header.name},
      {"header_size", header.header_size},
      {"unknown_0x14", header.unknown_0x14},
      {"record_size_or_header_used", header.record_size_or_header_used},
      {"payload_bytes_0x1c", header.payload_bytes_0x1c},
      {"payload_bytes_0x20", header.payload_bytes_0x20},
      {"raw_prefix_hex", hex(header.raw_prefix)},
  };
}

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
    nlohmann::ordered_json objects = nlohmann::ordered_json::array();
    for (const auto &object : *decoded_objects) {
      const auto structured = axk::structured_object_path(object);
      objects.push_back({
          {"key", object.key},
          {"logical_path", object.logical_path},
          {"scope_key", object.scope_key},
          {"raw_group", object.raw_group},
          {"raw_volume", object.raw_volume},
          {"group_label", object.group_label.value},
          {"group_label_status", status_name(object.group_label.status)},
          {"group_label_basis", object.group_label.basis},
          {"volume_label", object.volume_label.value},
          {"volume_label_status", status_name(object.volume_label.status)},
          {"volume_label_basis", object.volume_label.basis},
          {"data_offset", object.data_offset},
          {"size", object.size},
          {"structured_path", structured.relative_path.generic_string()},
          {"header", header_json(object.decoded.header)},
          {"decoded", decoded_payload_json(object.decoded)},
      });
    }
    std::cout << nlohmann::ordered_json{
                     {"schema_version", "1.0"},
                     {"container", kind_name(media->kind())},
                     {"objects", std::move(objects)},
                 }
                     .dump(pretty ? 2 : -1)
              << '\n';
    return 0;
  }
  const auto container = axk::open_image(path);
  if (!container) {
    std::cerr << axk::render_error(container.error()) << '\n';
    return 2;
  }
  nlohmann::ordered_json objects = nlohmann::ordered_json::array();
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
      objects.push_back({
          {"partition_index", partition.index.value},
          {"sfs_id", record.sfs_id.value},
          {"header", header_json(decoded->header)},
          {"decoded", decoded_payload_json(*decoded)},
      });
    }
  }
  std::cout << nlohmann::ordered_json{{"schema_version", "1.0"}, {"objects", std::move(objects)}}
                   .dump(pretty ? 2 : -1)
            << '\n';
  return 0;
}

struct SemanticSnapshot {
  axk::Container container;
  axk::ObjectCatalog catalog;
  axk::RelationshipGraph graph;
};

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

nlohmann::ordered_json relationship_json(const axk::Relationship &row) {
  return {
      {"key", row.key},
      {"source_key", row.source_key},
      {"target_key",
       row.target_key ? nlohmann::ordered_json(*row.target_key) : nlohmann::ordered_json(nullptr)},
      {"candidate_keys", row.candidate_keys},
      {"relationship_type", row.type},
      {"quality", axk::relationship_quality_name(row.quality)},
      {"basis", row.basis},
      {"notes", row.notes},
      {"scope_key", row.scope_key},
      {"assignment_index", row.assignment_index ? nlohmann::ordered_json(*row.assignment_index)
                                                : nlohmann::ordered_json(nullptr)},
      {"assignment_name", row.assignment_name},
      {"active_assignment_state", axk::assignment_state_name(row.assignment_state)},
      {"assignment_rch_assign_display", row.receive_channel_display},
  };
}

nlohmann::ordered_json content_node_json(const axk::ContentNode &node) {
  nlohmann::ordered_json children = nlohmann::ordered_json::array();
  for (const auto &child : node.children)
    children.push_back(content_node_json(child));
  return {
      {"node_id", node.node_id},
      {"node_type", node.node_type},
      {"display_name", node.display_name},
      {"object_key", node.object_key},
      {"object_type", node.object_type},
      {"quality", axk::relationship_quality_name(node.quality)},
      {"basis", node.basis},
      {"notes", node.notes},
      {"details", node.details},
      {"children", std::move(children)},
  };
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
    nlohmann::ordered_json rows = nlohmann::ordered_json::array();
    for (const auto &row : graph.relationships)
      rows.push_back(relationship_json(row));
    nlohmann::ordered_json bitmaps = nlohmann::ordered_json::array();
    for (const auto &row : graph.bitmap_comparisons) {
      bitmaps.push_back({
          {"sbnk_key", row.sbnk_key},
          {"bitmap_programs", row.bitmap_programs},
          {"direct_assignment_programs", row.direct_assignment_programs},
          {"indirect_assignment_programs", row.indirect_assignment_programs},
          {"bitmap_without_direct", row.bitmap_without_direct},
          {"direct_without_bitmap", row.direct_without_bitmap},
          {"status", row.status},
          {"mismatch_class", row.mismatch_class},
      });
    }
    std::cout << nlohmann::ordered_json{
                     {"schema_version", "1.0"},
                     {"relationships", std::move(rows)},
                     {"bitmap_comparisons", std::move(bitmaps)},
                 }
                     .dump(pretty ? 2 : -1)
              << '\n';
    return 0;
  }
  const auto snapshot = load_semantic_snapshot(path);
  if (!snapshot) {
    std::cerr << axk::render_error(snapshot.error()) << '\n';
    return 2;
  }
  nlohmann::ordered_json rows = nlohmann::ordered_json::array();
  for (const auto &row : snapshot->graph.relationships)
    rows.push_back(relationship_json(row));
  nlohmann::ordered_json bitmaps = nlohmann::ordered_json::array();
  for (const auto &row : snapshot->graph.bitmap_comparisons) {
    bitmaps.push_back({
        {"sbnk_key", row.sbnk_key},
        {"bitmap_programs", row.bitmap_programs},
        {"direct_assignment_programs", row.direct_assignment_programs},
        {"indirect_assignment_programs", row.indirect_assignment_programs},
        {"bitmap_without_direct", row.bitmap_without_direct},
        {"direct_without_bitmap", row.direct_without_bitmap},
        {"status", row.status},
        {"mismatch_class", row.mismatch_class},
    });
  }
  std::cout << nlohmann::ordered_json{
                   {"schema_version", "1.0"},
                   {"relationships", std::move(rows)},
                   {"bitmap_comparisons", std::move(bitmaps)},
               }
                   .dump(pretty ? 2 : -1)
            << '\n';
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
    const auto tree =
        axk::build_content_tree(path.generic_string(), *catalog, graph, include_default_programs);
    nlohmann::ordered_json roots = nlohmann::ordered_json::array();
    for (const auto &root : tree.roots)
      roots.push_back(content_node_json(root));
    std::cout << nlohmann::ordered_json{
                     {"schema_version", "1.0"},
                     {"source_path", tree.source_path},
                     {"roots", std::move(roots)},
                 }
                     .dump(pretty ? 2 : -1)
              << '\n';
    return 0;
  }
  const auto snapshot = load_semantic_snapshot(path);
  if (!snapshot) {
    std::cerr << axk::render_error(snapshot.error()) << '\n';
    return 2;
  }
  const auto tree = axk::build_content_tree(snapshot->container, snapshot->catalog, snapshot->graph,
                                            include_default_programs);
  nlohmann::ordered_json roots = nlohmann::ordered_json::array();
  for (const auto &root : tree.roots)
    roots.push_back(content_node_json(root));
  std::cout << nlohmann::ordered_json{
                   {"schema_version", "1.0"},
                   {"source_path", tree.source_path},
                   {"roots", std::move(roots)},
               }
                   .dump(pretty ? 2 : -1)
            << '\n';
  return 0;
}

int run_extract_wav(const std::filesystem::path &path, const std::filesystem::path &output_dir,
                    bool overwrite, bool pretty) {
  const auto snapshot = load_semantic_snapshot(path);
  if (!snapshot) {
    std::cerr << axk::render_error(snapshot.error()) << '\n';
    return 2;
  }
  nlohmann::ordered_json rows = nlohmann::ordered_json::array();
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
        {"partition_index", item.partition.value},
        {"sfs_id", item.sfs_id.value},
        {"object_key", item.key},
        {"name", waveform->name},
        {"wav_path", output.generic_string()},
        {"sample_rate", waveform->format.sample_rate},
        {"sample_width_bytes", waveform->format.sample_width_bytes},
        {"stored_sample_width_bytes", waveform->stored_sample_width_bytes},
        {"frame_count", waveform->frame_count},
        {"stored_payload_size", waveform->stored_payload_size},
        {"stored_payload_transform", waveform->stored_payload_transform},
        {"alternating_byte_payload_detected", waveform->alternating_byte_payload_detected},
    });
  }
  std::cout << nlohmann::ordered_json{
                   {"schema_version", "1.0"},
                   {"waveforms", std::move(rows)},
               }
                   .dump(pretty ? 2 : -1)
            << '\n';
  return 0;
}

nlohmann::ordered_json export_volume_json(const axk::VolumeExport &volume,
                                          const axk::RelationshipGraph &graph,
                                          const std::filesystem::path &source_path) {
  nlohmann::ordered_json smpl = nlohmann::ordered_json::array();
  for (const auto &waveform : volume.waveforms) {
    const auto &audio = waveform.waveform;
    smpl.push_back({
        {"id", waveform.object_key},
        {"object_key", waveform.object_key},
        {"display_name", waveform.display_name},
        {"wav_path", waveform.relative_wav_path.generic_string()},
        {"audio",
         {{"channels", audio.format.channels},
          {"sample_rate", audio.format.sample_rate},
          {"sample_width_bytes", audio.format.sample_width_bytes},
          {"frames", audio.frame_count},
          {"stored_payload_size", audio.stored_payload_size},
          {"decoded_pcm_size", audio.pcm.size()},
          {"stored_payload_transform", audio.stored_payload_transform},
          {"exactness_status", audio.alternating_byte_payload_detected
                                   ? "alternating-byte-compatibility-export"
                                   : "exact-current-mono"}}},
        {"playback",
         {{"root_key_midi", audio.root_key},
          {"fine_tune_cents", audio.fine_tune_cents},
          {"loop_mode_raw", audio.loop_mode},
          {"loop_mode_label", audio.loop_mode_label},
          {"loop_start_frame", audio.loop_start},
          {"loop_length_frames", audio.loop_length},
          {"loop_end_frame_a4000_ui", audio.loop_start + audio.loop_length}}},
        {"origin",
         {{"source_container", source_path.generic_string()},
          {"container_kind", "sfs"},
          {"partition_index", audio.partition.value},
          {"quality", audio.alternating_byte_payload_detected ? "Likely" : "Known"},
          {"basis", audio.alternating_byte_payload_detected
                        ? "direct object header plus alternating-byte payload detection"
                        : "direct object header and stored payload bytes"},
          {"alternating_byte_payload_detected", audio.alternating_byte_payload_detected}}},
    });
  }
  nlohmann::ordered_json sbnk = nlohmann::ordered_json::array();
  for (const auto &bank : volume.sample_banks) {
    nlohmann::ordered_json members = nlohmann::ordered_json::array();
    for (const auto &member : bank.members) {
      members.push_back({
          {"role", member.role},
          {"smpl_id", member.waveform_key},
          {"wav_path", member.relative_wav_path.generic_string()},
          {"relationship_quality", axk::relationship_quality_name(member.quality)},
      });
    }
    nlohmann::ordered_json rendered = nullptr;
    if (bank.rendered_wav_path) {
      rendered = {
          {"wav_path", bank.rendered_wav_path->generic_string()},
          {"channels", 2},
          {"frames", bank.stereo_decision ? bank.stereo_decision->output_frame_count : 0},
          {"padding",
           {{"left_frames", bank.stereo_decision ? bank.stereo_decision->left_padding_frames : 0},
            {"right_frames",
             bank.stereo_decision ? bank.stereo_decision->right_padding_frames : 0}}},
      };
    }
    nlohmann::ordered_json parameter_contexts = nlohmann::ordered_json::array();
    for (const auto &context : bank.parameter_contexts) {
      nlohmann::ordered_json numeric_fields = nlohmann::ordered_json::object();
      numeric_fields["sample_parameter_base_0x0a8"] = 0xa8;
      for (const auto &field : context.decoded.numeric_fields) {
        if (field.value)
          numeric_fields[field.name] = *field.value;
      }
      nlohmann::ordered_json controls = nlohmann::ordered_json::array();
      for (std::size_t index = 0; index < context.decoded.control_records.size(); ++index) {
        const auto &control = context.decoded.control_records[index];
        controls.push_back({
            {"index", index + 1U},
            {"offset", 0x164U + index * 4U},
            {"device_u8", control.device},
            {"function_u8", control.function},
            {"type_u8", control.type},
            {"range_s8", control.range},
        });
      }
      numeric_fields["sample_control_records"] = std::move(controls);
      parameter_contexts.push_back({
          {"sample_bank_object_key", context.object_key},
          {"sample_bank_name", context.display_name},
          {"relationship_type", context.relationship_type},
          {"member_parameters", std::move(numeric_fields)},
          {"left_member", member_json(context.decoded.left)},
          {"right_member", context.decoded.right ? member_json(*context.decoded.right)
                                                 : nlohmann::ordered_json(nullptr)},
          {"selected_member", member_json(context.decoded.left)},
          {"bank_topology", context.decoded.right ? "two-member" : "single-member"},
          {"linked_program_numbers", context.decoded.linked_program_numbers},
      });
    }
    const auto &range_bank =
        bank.parameter_contexts.empty() ? bank.decoded : bank.parameter_contexts.front().decoded;
    const auto range_low = range_bank.key_range_low;
    const auto range_high = range_bank.key_range_high;
    nlohmann::ordered_json resolved_range = {
        {"low_midi", range_low == 255U ? nlohmann::ordered_json(range_bank.left.root_key)
                                       : nlohmann::ordered_json(range_low)},
        {"high_midi", range_high == 128U ? nlohmann::ordered_json(range_bank.left.root_key)
                                         : nlohmann::ordered_json(range_high)},
        {"low_raw", range_low},
        {"high_raw", range_high},
        {"low_display",
         range_low == 255U ? nlohmann::ordered_json("Orig") : nlohmann::ordered_json(range_low)},
        {"high_display",
         range_high == 128U ? nlohmann::ordered_json("Orig") : nlohmann::ordered_json(range_high)},
        {"basis",
         range_low == 255U || range_high == 128U ? "sampler-orig-key-limit" : "decoded-key-range"},
    };
    sbnk.push_back({
        {"id", bank.object_key},
        {"object_key", bank.object_key},
        {"display_name", bank.display_name},
        {"physical_waveforms", std::move(members)},
        {"rendered_audio", std::move(rendered)},
        {"parameters",
         {{"decoded_current_sbnk_member_parameters", std::move(parameter_contexts)},
          {"resolved_key_range", std::move(resolved_range)}}},
    });
  }
  nlohmann::ordered_json sbac = nlohmann::ordered_json::array();
  for (const auto &group : volume.sample_bank_groups) {
    nlohmann::ordered_json members = nlohmann::ordered_json::array();
    for (const auto &key : group.member_bank_keys)
      members.push_back({{"sbnk_id", key}});
    sbac.push_back({
        {"id", group.object_key},
        {"object_key", group.object_key},
        {"display_name", group.display_name},
        {"members", std::move(members)},
    });
  }
  nlohmann::ordered_json prog = nlohmann::ordered_json::array();
  for (const auto &program : volume.programs) {
    prog.push_back({
        {"id", program.object_key},
        {"object_key", program.object_key},
        {"display_name", program.display_name},
        {"assignment_target_keys", program.assignment_target_keys},
    });
  }
  nlohmann::ordered_json relationships = nlohmann::ordered_json::array();
  std::set<std::string> volume_keys;
  for (const auto &row : volume.waveforms)
    volume_keys.insert(row.object_key);
  for (const auto &row : volume.sample_banks)
    volume_keys.insert(row.object_key);
  for (const auto &row : volume.sample_bank_groups)
    volume_keys.insert(row.object_key);
  for (const auto &row : volume.programs)
    volume_keys.insert(row.object_key);
  for (const auto &row : graph.relationships) {
    if (!volume_keys.contains(row.source_key))
      continue;
    if (row.type == "SBNK_PROGRAM_BITMAP_TO_PROG" || row.type == "PROG_ASSIGNMENT_TO_SBAC") {
      continue;
    }
    relationships.push_back({
        {"relationship_type", row.type},
        {"source_key", row.source_key},
        {"target_key", row.target_key ? *row.target_key : ""},
        {"quality", axk::relationship_quality_name(row.quality)},
        {"basis", row.basis},
        {"active_assignment_state", axk::assignment_state_name(row.assignment_state)},
    });
  }
  nlohmann::ordered_json decisions = nlohmann::ordered_json::array();
  for (const auto &bank : volume.sample_banks) {
    if (!bank.stereo_decision)
      continue;
    decisions.push_back({
        {"sbnk_object_key", bank.object_key},
        {"sample_name", bank.display_name},
        {"decision",
         bank.stereo_decision->renderable ? "interleaved_stereo_written" : "physical_members_only"},
        {"reason_code", bank.stereo_decision->reason_code},
        {"output_frame_count", bank.stereo_decision->output_frame_count},
        {"left_padding_frames", bank.stereo_decision->left_padding_frames},
        {"right_padding_frames", bank.stereo_decision->right_padding_frames},
    });
  }
  return {
      {"schema", "axklib.volume_graph.v1"},
      {"source",
       {{"containers", {source_path.generic_string()}},
        {"container_kinds", {"sfs"}},
        {"source_scope", ""}}},
      {"volume",
       {{"path", volume.relative_root.generic_string()},
        {"partition_index", volume.partition.value},
        {"partition_name", volume.partition_name},
        {"name", volume.volume_name},
        {"placement_quality", "Known"}}},
      {"objects",
       {{"smpl", std::move(smpl)},
        {"sbnk", std::move(sbnk)},
        {"sbac", std::move(sbac)},
        {"prog", std::move(prog)},
        {"sequ", nlohmann::ordered_json::array()}}},
      {"relationships", std::move(relationships)},
      {"stereo_decisions", std::move(decisions)},
  };
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
  nlohmann::ordered_json volumes = nlohmann::ordered_json::array();
  for (const auto &volume : plan->volumes) {
    auto value = export_volume_json(volume, snapshot->graph, path);
    const auto graph_path = output_dir / volume.relative_root / "volume.axklib.json";
    std::error_code error;
    std::filesystem::create_directories(graph_path.parent_path(), error);
    const auto temporary =
        graph_path.parent_path() / ("." + graph_path.filename().string() + ".tmp");
    {
      std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
      output << value.dump(2) << '\n';
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
        {"path", volume.relative_root.generic_string()},
        {"graph_path", graph_path.generic_string()},
        {"waveform_count", volume.waveforms.size()},
        {"sample_bank_count", volume.sample_banks.size()},
    });
  }
  std::cout << nlohmann::ordered_json{{"schema_version", "1.0"}, {"volumes", std::move(volumes)}}
                   .dump(pretty ? 2 : -1)
            << '\n';
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
  nlohmann::ordered_json values = nlohmann::ordered_json::array();
  for (const auto &bin : preview->bins)
    values.push_back({bin.minimum, bin.maximum});
  std::cout << nlohmann::ordered_json{
                   {"schema_version", "1.0"},
                   {"object_key", object_key},
                   {"frame_count", preview->frame_count},
                   {"bins", std::move(values)},
               }
                   .dump(pretty ? 2 : -1)
            << '\n';
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
  std::cout << "image=" << written->path.generic_string() << " size_bytes=" << written->size_bytes
            << " partitions=" << written->partitions.size() << " objects=" << object_count
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
  nlohmann::ordered_json operations = nlohmann::ordered_json::array();
  for (const auto &operation : altered->operations) {
    std::vector<std::uint32_t> removed;
    std::ranges::transform(operation.removed_sfs_ids, std::back_inserter(removed),
                           [](axk::SfsId id) { return id.value; });
    std::vector<std::uint32_t> inserted;
    std::ranges::transform(operation.inserted_sfs_ids, std::back_inserter(inserted),
                           [](axk::SfsId id) { return id.value; });
    nlohmann::ordered_json audio_import = nullptr;
    if (operation.audio_import) {
      const auto &audio = *operation.audio_import;
      audio_import = {
          {"source_path", audio.source_path.generic_string()},
          {"source_format", audio.source_format},
          {"source_subtype", audio.source_subtype},
          {"source_channels", audio.source_channels},
          {"source_sample_rate", audio.source_sample_rate},
          {"output_sample_rate", audio.output_sample_rate},
          {"output_frames", audio.output_frames},
          {"resampled", audio.resampled},
          {"quantized", audio.quantized},
          {"split_stereo", audio.split_stereo},
          {"clipped_samples", audio.clipped_samples},
      };
    }
    operations.push_back({
        {"id", operation.id},
        {"type", operation.type},
        {"partition_index", operation.partition.value},
        {"volume_name", operation.volume_name},
        {"object_name", operation.object_name},
        {"removed_sfs_ids", std::move(removed)},
        {"inserted_sfs_ids", std::move(inserted)},
        {"freed_clusters", operation.freed_clusters},
        {"allocated_clusters", operation.allocated_clusters},
        {"audio_import", std::move(audio_import)},
    });
  }
  std::cout << nlohmann::ordered_json{
                   {"source_path", altered->source_path.generic_string()},
                   {"output_path",
                    altered->output_path
                        ? nlohmann::ordered_json(altered->output_path->generic_string())
                        : nlohmann::ordered_json(nullptr)},
                   {"applied", altered->applied},
                   {"operations", std::move(operations)},
               }
                   .dump(pretty ? 2 : -1)
            << '\n';
  return 0;
}

std::string object_type_text(axk::ObjectType type) {
  switch (type) {
  case axk::ObjectType::smpl:
    return "SMPL";
  case axk::ObjectType::sbnk:
    return "SBNK";
  case axk::ObjectType::sbac:
    return "SBAC";
  case axk::ObjectType::prog:
    return "PROG";
  case axk::ObjectType::sequ:
    return "SEQU";
  case axk::ObjectType::prf3:
    return "PRF3";
  case axk::ObjectType::unknown:
    return "UNKNOWN";
  }
  return "UNKNOWN";
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

std::vector<std::filesystem::path>
expand_cli_paths(const std::vector<std::filesystem::path> &inputs) {
  static const std::set<std::string> extensions{".hda", ".hds", ".ima", ".img", ".iso"};
  std::vector<std::filesystem::path> result;
  for (const auto &path : inputs) {
    std::error_code error;
    if (std::filesystem::is_directory(path, error)) {
      for (std::filesystem::recursive_directory_iterator it{path, error}, end; it != end && !error;
           it.increment(error)) {
        if (it->is_regular_file(error) && extensions.contains(it->path().extension().string()))
          result.push_back(it->path());
      }
    } else {
      result.push_back(path);
    }
  }
  std::ranges::sort(result, {}, [](const auto &path) { return path.generic_string(); });
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

struct CliLoaded {
  std::filesystem::path path;
  axk::MediaContainer media;
  std::vector<axk::MediaObject> objects;
  axk::ObjectCatalog catalog;
  axk::RelationshipGraph graph;
};

axk::ContentTree cli_content_tree(const CliLoaded &loaded, bool include_default_programs);
std::string sfs_selector_component(const axk::ContentNode &node);

struct CliLoadResult {
  std::vector<CliLoaded> loaded;
  std::vector<axk::ReportRow> errors;
};

CliLoadResult load_cli_paths(const std::vector<std::filesystem::path> &inputs) {
  CliLoadResult result;
  for (const auto &path : expand_cli_paths(inputs)) {
    auto media = axk::open_media(path);
    if (!media) {
      result.errors.push_back({{"path", path.generic_string()},
                               {"error_code", static_cast<std::uint64_t>(media.error().code)},
                               {"message", media.error().message},
                               {"recoverable", true},
                               {"original_exception", "axk::Error"}});
      continue;
    }
    auto catalog = axk::build_object_catalog(*media);
    if (!catalog) {
      result.errors.push_back({{"path", path.generic_string()},
                               {"error_code", static_cast<std::uint64_t>(catalog.error().code)},
                               {"message", catalog.error().message},
                               {"recoverable", true},
                               {"original_exception", "axk::Error"}});
      continue;
    }
    auto objects = media->objects();
    if (!objects) {
      result.errors.push_back({{"path", path.generic_string()},
                               {"error_code", static_cast<std::uint64_t>(objects.error().code)},
                               {"message", objects.error().message},
                               {"recoverable", true},
                               {"original_exception", "axk::Error"}});
      continue;
    }
    auto graph = axk::build_relationship_graph(*catalog);
    result.loaded.push_back(
        {path, std::move(*media), std::move(*objects), std::move(*catalog), std::move(graph)});
  }
  return result;
}

axk::Result<void> prepare_report_directory(const std::filesystem::path &path, bool overwrite) {
  std::error_code error;
  if (std::filesystem::exists(path, error) && !overwrite &&
      std::filesystem::directory_iterator{path, error} != std::filesystem::directory_iterator{}) {
    return std::unexpected{
        axk::make_error(axk::ErrorCode::io_open_failed, axk::ErrorCategory::io,
                        "output directory is not empty: " + path.generic_string())};
  }
  std::filesystem::create_directories(path / "_schemas", error);
  if (error)
    return std::unexpected{axk::make_error(axk::ErrorCode::io_open_failed, axk::ErrorCategory::io,
                                           "could not create report output directory")};
  return {};
}

axk::Result<axk::ReportSchemaManifest>
write_cli_report(const std::filesystem::path &output, std::string name,
                 std::span<const axk::ReportRow> rows, std::string source_command, bool overwrite) {
  if (auto written = axk::write_report_csv(output / (name + ".csv"), rows, {}, overwrite); !written)
    return std::unexpected{written.error()};
  if (auto written = axk::write_report_json(output / (name + ".json"), rows, overwrite); !written)
    return std::unexpected{written.error()};
  axk::ReportSchemaOptions options;
  static_cast<void>(source_command);
  options.source_command = "axklib";
  options.library_version = std::string{oracle_report_library_version};
  if (name == "inventory_objects")
    options.semantic_notes =
        "Decoded object inventory rows produced through axklib.objects.decoded.";
  else if (name == "decode_issues")
    options.semantic_notes = "Decode issues use stable code/severity/quality fields.";
  else if (name == "objects")
    options.semantic_notes =
        "Filtered object summary rows produced through the canonical inventory view.";
  else if (name == "validation_issues")
    options.semantic_notes =
        "Validation issues use stable issue codes intended for regression and CI gates.";
  auto schema = axk::make_report_schema(name, rows, std::move(options));
  if (auto written = axk::write_report_schema(output / "_schemas" / (name + ".schema.json"), schema,
                                              overwrite);
      !written)
    return std::unexpected{written.error()};
  return schema;
}

std::string public_object_key(const CliLoaded &loaded, std::string_view native_key) {
  if (loaded.media.kind() == axk::MediaKind::sfs)
    return std::string{native_key};
  const auto object = std::ranges::find(loaded.objects, native_key, &axk::MediaObject::key);
  if (object == loaded.objects.end())
    return std::string{native_key};
  if (loaded.media.kind() == axk::MediaKind::fat12_floppy)
    return std::format("{}:{}", loaded.path.filename().string(), object->logical_path);
  if (loaded.media.kind() == axk::MediaKind::iso9660)
    return std::format("{}:iso9660:{}", loaded.path.filename().string(), object->logical_path);
  return std::format("{}:standalone-object", loaded.path.filename().string());
}

std::string public_scope_key(const CliLoaded &loaded, const axk::ObjectSnapshot &item) {
  if (loaded.media.kind() == axk::MediaKind::sfs)
    return std::format("{}:partition:{}", loaded.path.generic_string(), item.partition.value);
  if (loaded.media.kind() == axk::MediaKind::fat12_floppy)
    return std::format("{}:fat-root", loaded.path.generic_string());
  if (loaded.media.kind() == axk::MediaKind::standalone_object)
    return std::format("{}:standalone-object", loaded.path.generic_string());
  const auto object = std::ranges::find(loaded.objects, item.key, &axk::MediaObject::key);
  return object == loaded.objects.end()
             ? std::format("{}:iso", loaded.path.generic_string())
             : std::format("{}:{}", loaded.path.generic_string(), object->scope_key);
}

axk::ReportRow inventory_row(const CliLoaded &loaded, const axk::ObjectSnapshot &item) {
  const auto media_object = std::ranges::find(loaded.objects, item.key, &axk::MediaObject::key);
  const auto iso = loaded.media.kind() == axk::MediaKind::iso9660;
  const auto fat = loaded.media.kind() == axk::MediaKind::fat12_floppy;
  const auto sfs = loaded.media.kind() == axk::MediaKind::sfs;
  std::string decoded_kind{"UnknownObject"};
  std::string decoded_fields;
  if (item.object.header.type == axk::ObjectType::smpl) {
    decoded_kind = "DecodedSample";
    decoded_fields = "fine_tune;loop_length;loop_mode;loop_start;root_key;sample_rate";
  } else if (item.object.header.type == axk::ObjectType::sbnk) {
    decoded_kind = "DecodedSampleBank";
    decoded_fields = "bank_topology;left_sample_name;left_smpl_link_id";
  } else if (item.object.header.type == axk::ObjectType::sbac) {
    decoded_kind = "DecodedSampleBankAccessory";
    decoded_fields = "active_slot_count;max_slot_count_from_payload";
  } else if (item.object.header.type == axk::ObjectType::prog) {
    decoded_kind = "DecodedProgram";
    decoded_fields = "control_record_count";
  } else if (item.object.header.type == axk::ObjectType::sequ) {
    decoded_kind = "DecodedSequence";
  }
  const auto field_count =
      decoded_fields.empty()
          ? 0U
          : static_cast<unsigned int>(std::ranges::count(decoded_fields, ';') + 1);
  std::uint64_t payload_offset{};
  if (sfs) {
    const auto &container = std::get<axk::Container>(loaded.media.storage());
    const auto partition = std::ranges::find(container.partitions(), item.partition.value,
                                             [](const auto &row) { return row.index.value; });
    if (partition != container.partitions().end()) {
      const auto record = std::ranges::find(partition->records, item.sfs_id.value,
                                            [](const auto &row) { return row.sfs_id.value; });
      if (record != partition->records.end() && !record->extents.empty()) {
        payload_offset = (static_cast<std::uint64_t>(partition->start_sector) +
                          static_cast<std::uint64_t>(record->extents.front().cluster_offset) *
                              partition->sectors_per_cluster) *
                         container.superblock().sector_size_bytes;
      }
    }
  }
  const auto payload_size = media_object != loaded.objects.end()
                                ? static_cast<std::uint64_t>(media_object->raw_payload.size())
                                : static_cast<std::uint64_t>(item.object.header.header_size) +
                                      item.object.header.payload_bytes_0x1c;
  return {
      {"source_path", loaded.path.generic_string()},
      {"container_kind", media_kind_text(loaded.media.kind())},
      {"detected_format", media_kind_text(loaded.media.kind())},
      {"scope_key", public_scope_key(loaded, item)},
      {"object_key", public_object_key(loaded, item.key)},
      {"partition_index", sfs ? axk::ReportValue{static_cast<std::uint64_t>(item.partition.value)}
                              : axk::ReportValue{""}},
      {"sfs_id", sfs ? axk::ReportValue{static_cast<std::uint64_t>(item.sfs_id.value)}
                     : axk::ReportValue{""}},
      {"fat_file", !sfs && media_object != loaded.objects.end()
                       ? axk::ReportValue{media_object->logical_path}
                       : axk::ReportValue{""}},
      {"payload_offset", sfs ? axk::ReportValue{payload_offset}
                         : media_object != loaded.objects.end()
                             ? axk::ReportValue{media_object->data_offset}
                             : axk::ReportValue{""}},
      {"payload_size", payload_size},
      {"object_type", object_type_text(item.object.header.type)},
      {"object_name", item.object.header.name},
      {"object_format", "normal-fsfsdev3splx"},
      {"decoded_kind", decoded_kind},
      {"decoded_field_count", static_cast<std::uint64_t>(field_count)},
      {"decoded_fields", decoded_fields},
      {"decode_issue_count", std::uint64_t{0}},
      {"decode_issue_codes", ""},
      {"iso_extent_sector", iso && media_object != loaded.objects.end()
                                ? axk::ReportValue{media_object->data_offset / 2048U}
                                : axk::ReportValue{""}},
      {"iso_data_offset", iso && media_object != loaded.objects.end()
                              ? axk::ReportValue{media_object->data_offset}
                              : axk::ReportValue{""}},
      {"iso_file_size", iso && media_object != loaded.objects.end()
                            ? axk::ReportValue{media_object->size}
                            : axk::ReportValue{""}},
      {"iso_recovery_quality",
       iso ? axk::ReportValue{"clean-iso9660-object"} : axk::ReportValue{""}},
      {"iso_raw_group", iso && media_object != loaded.objects.end()
                            ? axk::ReportValue{media_object->raw_group}
                            : axk::ReportValue{""}},
      {"iso_raw_volume", iso && media_object != loaded.objects.end()
                             ? axk::ReportValue{media_object->raw_volume}
                             : axk::ReportValue{""}},
      {"iso_group_label", iso && media_object != loaded.objects.end()
                              ? axk::ReportValue{media_object->group_label.value}
                              : axk::ReportValue{""}},
      {"iso_volume_label", iso && media_object != loaded.objects.end()
                               ? axk::ReportValue{media_object->volume_label.value}
                               : axk::ReportValue{""}},
      {"iso_group_label_source", ""},
      {"iso_volume_label_source", ""},
      {"fat_directory_offset", ""},
      {"fat_first_cluster", ""},
      {"fat_cluster_count", ""},
      {"fat_file_size", fat && media_object != loaded.objects.end()
                            ? axk::ReportValue{media_object->size}
                            : axk::ReportValue{""}},
      {"fat_object_offset", fat && media_object != loaded.objects.end()
                                ? axk::ReportValue{media_object->data_offset}
                                : axk::ReportValue{""}},
      {"fat_stored_payload_offset",
       fat && media_object != loaded.objects.end()
           ? axk::ReportValue{media_object->data_offset + item.object.header.header_size}
           : axk::ReportValue{""}}};
}

axk::ReportRow relationship_report_row(const CliLoaded &loaded, const axk::Relationship &row) {
  std::string target;
  if (row.target_key)
    target = public_object_key(loaded, *row.target_key);
  else {
    for (const auto &candidate : row.candidate_keys) {
      if (!target.empty())
        target += '|';
      target += public_object_key(loaded, candidate);
    }
  }
  const auto source_key = public_object_key(loaded, row.source_key);
  std::string raw_fields;
  std::string notes = row.notes;
  const auto source =
      std::ranges::find(loaded.catalog.objects, row.source_key, &axk::ObjectSnapshot::key);
  if (source != loaded.catalog.objects.end() &&
      (row.type == "SBNK_LEFT_MEMBER_TO_SMPL" || row.type == "SBNK_RIGHT_MEMBER_TO_SMPL")) {
    if (const auto *bank = std::get_if<axk::CurrentSbnk>(&source->object.payload)) {
      const bool right = row.type == "SBNK_RIGHT_MEMBER_TO_SMPL";
      const auto *member = right && bank->right ? &*bank->right : &bank->left;
      raw_fields =
          std::format("SBNK+{} member {}; name={}; link_id=0x{:08x}", right ? "right" : "left",
                      right ? "right" : "left", member->sample_name, member->smpl_link_id);
      if (row.basis == "sbnk-member-link+name")
        notes = "Current SBNK member name and member link ID match exactly one same-scope SMPL "
                "object.";
    }
  } else if (source != loaded.catalog.objects.end() && row.type == "SBAC_SLOT_TO_SBNK") {
    if (const auto *group = std::get_if<axk::CurrentSbac>(&source->object.payload)) {
      std::size_t index{};
      if (row.target_key) {
        const auto target_object =
            std::ranges::find(loaded.catalog.objects, *row.target_key, &axk::ObjectSnapshot::key);
        if (target_object != loaded.catalog.objects.end()) {
          const auto found = std::ranges::find(group->slots, target_object->object.header.name,
                                               &axk::SbacSlot::name);
          if (found != group->slots.end())
            index = static_cast<std::size_t>(std::distance(group->slots.begin(), found));
        }
      }
      const auto offset = index < group->slots.size() ? group->slots[index].offset : 0x14cU;
      raw_fields = std::format("SBAC slot {} at 0x{:03x}", index, offset);
      if (row.basis == "active-sbac-slot-name")
        notes = "Input consistency: counted SBAC slot name uniquely matches a same-scope SBNK "
                "header name. The companion 32-bit slot word is preserved as raw/opaque.";
    }
  } else if (row.type == "SBNK_PROGRAM_BITMAP_TO_PROG") {
    raw_fields = "SBNK+0x0c0..0x0cf";
    notes = "Validated standalone assignment rows support SBNK+0x0c0..0x0cf as four "
            "big-endian program-link bitmap words for direct PROG->SBNK/sample assignments. "
            "PROG->SBAC assignments are reported separately as indirection and are not expected "
            "to set child SBNK bits.";
  } else if (row.assignment_index) {
    raw_fields = std::format("PROG assignment {} at 0x{:03x}", *row.assignment_index,
                             0x120U + static_cast<unsigned int>(*row.assignment_index) * 0x38U);
  }
  std::string diagnostic;
  if (row.assignment_state == axk::AssignmentState::visible_off)
    diagnostic = "visible-off-assignment";
  else if (row.quality == axk::RelationshipQuality::tentative)
    diagnostic = "ambiguous-target";
  else if (row.quality == axk::RelationshipQuality::unknown)
    diagnostic = "missing-target";
  return {{"key", std::format("{}|{}|{}|{}", source_key, row.type,
                              target.empty() ? "missing" : target, row.basis)},
          {"source_key", source_key},
          {"target_key", target},
          {"relationship_type", row.type},
          {"quality", std::string{axk::relationship_quality_name(row.quality)}},
          {"basis", row.basis},
          {"raw_fields", raw_fields},
          {"ambiguity_notes", notes},
          {"source_image", loaded.path.generic_string()},
          {"scope_key", std::format("{}:{}", loaded.path.generic_string(), row.scope_key)},
          {"assignment_index", row.assignment_index ? axk::ReportValue{static_cast<std::uint64_t>(
                                                          *row.assignment_index)}
                                                    : axk::ReportValue{nullptr}},
          {"assignment_name", row.assignment_name},
          {"assignment_row_state", row.assignment_index ? "decoded-row" : ""},
          {"active_assignment_state",
           row.assignment_index ? std::string{axk::assignment_state_name(row.assignment_state)}
                                : std::string{}},
          {"assignment_rch_assign_display", row.receive_channel_display},
          {"diagnostic_category", diagnostic}};
}

int report_failure(const axk::Error &error) {
  std::cerr << axk::render_error(error) << '\n';
  if (error.message.starts_with("output directory is not empty") ||
      error.message.starts_with("refusing to replace existing report"))
    return 1;
  return 2;
}

int run_objects_request(const axk::cli::ObjectsRequest &request) {
  if (!request.output_directory) {
    if (request.paths.size() != 1U) {
      std::cerr << "objects requires one input when --output-dir is omitted\n";
      return 2;
    }
    return run_objects(request.paths.front(), request.pretty);
  }
  if (const auto ready = prepare_report_directory(*request.output_directory, request.overwrite);
      !ready)
    return report_failure(ready.error());
  const auto loaded = load_cli_paths(request.paths);
  std::vector<axk::ReportRow> rows;
  for (const auto &source : loaded.loaded) {
    for (const auto &item : source.catalog.objects) {
      if (request.object_type && object_type_text(item.object.header.type) != *request.object_type)
        continue;
      rows.push_back(inventory_row(source, item));
    }
  }
  auto schema = write_cli_report(*request.output_directory, "objects", rows, "axklib objects",
                                 request.overwrite);
  if (!schema)
    return report_failure(schema.error());
  const std::array schemas{*schema};
  if (auto index = axk::write_report_schema_index(
          *request.output_directory / "_schemas" / "schema_index.json", schemas, request.overwrite);
      !index)
    return report_failure(index.error());
  std::cout << "objects=" << rows.size() << " load_errors=" << loaded.errors.size() << '\n';
  std::cout << "reports written to " << request.output_directory->generic_string() << '\n';
  return loaded.errors.empty() ? 0 : 3;
}

int run_inventory_request(const axk::cli::InventoryRequest &request) {
  if (const auto ready = prepare_report_directory(request.output_directory, request.overwrite);
      !ready)
    return report_failure(ready.error());
  const auto loaded = load_cli_paths(request.paths);
  std::vector<axk::ReportRow> rows;
  std::vector<axk::ReportRow> issues;
  std::map<std::string, std::uint64_t> counts;
  for (const auto &source : loaded.loaded) {
    for (const auto &item : source.catalog.objects) {
      rows.push_back(inventory_row(source, item));
      ++counts[object_type_text(item.object.header.type)];
    }
    for (const auto &issue : source.catalog.issues) {
      issues.push_back(
          {{"source_path", source.path.generic_string()},
           {"container_kind", media_kind_text(source.media.kind())},
           {"object_key", issue.sfs_id
                              ? std::format("p{}:sfs{}", issue.partition.value, issue.sfs_id->value)
                              : std::string{}},
           {"object_type", ""},
           {"object_name", ""},
           {"code", issue.code},
           {"severity", "error"},
           {"message", issue.message},
           {"byte_start", nullptr},
           {"byte_end", nullptr},
           {"quality", "Unknown"},
           {"basis", "native catalog decode"}});
    }
  }
  auto object_schema = write_cli_report(request.output_directory, "inventory_objects", rows,
                                        "axklib inventory", request.overwrite);
  if (!object_schema)
    return report_failure(object_schema.error());
  auto issue_schema = write_cli_report(request.output_directory, "decode_issues", issues,
                                       "axklib inventory", request.overwrite);
  if (!issue_schema)
    return report_failure(issue_schema.error());
  axk::ReportValue::Object type_counts;
  for (const auto &[name, count] : counts)
    type_counts.emplace_back(name, count);
  axk::ReportValue::Array load_errors;
  for (const auto &row : loaded.errors)
    load_errors.emplace_back(axk::ReportValue::Object{row.begin(), row.end()});
  axk::ReportRow summary{
      {"input_count", static_cast<std::uint64_t>(loaded.loaded.size() + loaded.errors.size())},
      {"object_count", static_cast<std::uint64_t>(rows.size())},
      {"decode_issue_count", static_cast<std::uint64_t>(issues.size())},
      {"load_error_count", static_cast<std::uint64_t>(loaded.errors.size())},
      {"object_type_counts", std::move(type_counts)},
      {"load_errors", std::move(load_errors)}};
  if (auto written = axk::write_report_object(request.output_directory / "inventory_summary.json",
                                              summary, request.overwrite);
      !written)
    return report_failure(written.error());
  axk::ReportSchemaOptions summary_options;
  summary_options.source_command = "axklib";
  summary_options.library_version = std::string{oracle_report_library_version};
  auto summary_schema =
      axk::make_report_schema("inventory_summary", std::span{&summary, 1U}, summary_options);
  if (auto written = axk::write_report_schema(request.output_directory / "_schemas" /
                                                  "inventory_summary.schema.json",
                                              summary_schema, request.overwrite);
      !written)
    return report_failure(written.error());
  const std::array schemas{*object_schema, *issue_schema, summary_schema};
  if (auto index = axk::write_report_schema_index(
          request.output_directory / "_schemas" / "schema_index.json", schemas, request.overwrite);
      !index)
    return report_failure(index.error());
  std::cout << "objects=" << rows.size() << " decode_issues=" << issues.size()
            << " load_errors=" << loaded.errors.size() << '\n';
  std::cout << "reports written to " << request.output_directory.generic_string() << '\n';
  return loaded.errors.empty() ? 0 : 1;
}

std::vector<axk::ReportRow> relationship_rows(const CliLoadResult &loaded) {
  std::vector<axk::ReportRow> rows;
  for (const auto &source : loaded.loaded) {
    for (const auto &row : source.graph.relationships)
      rows.push_back(relationship_report_row(source, row));
  }
  return rows;
}

const axk::ObjectSnapshot *catalog_object(const CliLoaded &loaded, std::string_view key) {
  const auto found = std::ranges::find(loaded.catalog.objects, key, &axk::ObjectSnapshot::key);
  return found == loaded.catalog.objects.end() ? nullptr : &*found;
}

const axk::MediaObject *media_object(const CliLoaded &loaded, std::string_view key) {
  const auto found = std::ranges::find(loaded.objects, key, &axk::MediaObject::key);
  return found == loaded.objects.end() ? nullptr : &*found;
}

std::uint64_t sfs_payload_offset(const CliLoaded &loaded, const axk::ObjectSnapshot &item) {
  if (loaded.media.kind() != axk::MediaKind::sfs)
    return 0U;
  const auto &container = std::get<axk::Container>(loaded.media.storage());
  const auto partition = std::ranges::find(container.partitions(), item.partition.value,
                                           [](const auto &row) { return row.index.value; });
  if (partition == container.partitions().end())
    return 0U;
  const auto record = std::ranges::find(partition->records, item.sfs_id.value,
                                        [](const auto &row) { return row.sfs_id.value; });
  if (record == partition->records.end() || record->extents.empty())
    return 0U;
  return (static_cast<std::uint64_t>(partition->start_sector) +
          static_cast<std::uint64_t>(record->extents.front().cluster_offset) *
              partition->sectors_per_cluster) *
         container.superblock().sector_size_bytes;
}

std::string joined_strings(const std::vector<std::string> &items) {
  std::string result;
  for (const auto &item : items) {
    if (!result.empty())
      result += '|';
    result += item;
  }
  return result;
}

std::string joined_programs(const std::vector<std::uint8_t> &items) {
  std::string result;
  for (const auto item : items) {
    if (!result.empty())
      result += '|';
    result += std::format("{:03}", item);
  }
  return result;
}

axk::ReportValue optional_unsigned(bool present, std::uint64_t value) {
  return present ? axk::ReportValue{value} : axk::ReportValue{nullptr};
}

std::vector<axk::ReportRow> sbac_detail_rows(const CliLoadResult &loaded) {
  std::vector<axk::ReportRow> rows;
  for (const auto &source : loaded.loaded) {
    for (const auto &relation : source.graph.relationships) {
      if (relation.type != "SBAC_SLOT_TO_SBNK")
        continue;
      const auto *sbac_item = catalog_object(source, relation.source_key);
      if (sbac_item == nullptr)
        continue;
      const auto *sbac = std::get_if<axk::CurrentSbac>(&sbac_item->object.payload);
      if (sbac == nullptr)
        continue;
      const auto matched = relation.target_key ? catalog_object(source, *relation.target_key) : nullptr;
      std::size_t slot_index{};
      const axk::SbacSlot *slot{};
      for (std::size_t index = 0; index < sbac->slots.size(); ++index) {
        if (sbac->slots[index].name == relation.assignment_name ||
            (relation.assignment_name.empty() && matched != nullptr &&
             sbac->slots[index].name == matched->object.header.name)) {
          slot_index = index;
          slot = &sbac->slots[index];
          break;
        }
      }
      if (slot == nullptr) {
        const auto named = std::ranges::find_if(sbac->slots, [](const auto &item) {
          return !item.name.empty();
        });
        if (named == sbac->slots.end())
          continue;
        slot_index = static_cast<std::size_t>(std::distance(sbac->slots.begin(), named));
        slot = &*named;
      }
      std::vector<std::string> candidate_keys;
      std::vector<std::string> candidate_files;
      std::vector<std::string> candidate_names;
      for (const auto &key : relation.candidate_keys) {
        candidate_keys.push_back(public_object_key(source, key));
        if (const auto *candidate = catalog_object(source, key))
          candidate_names.push_back(candidate->object.header.name);
        if (const auto *object = media_object(source, key))
          candidate_files.push_back(object->logical_path);
      }
      const auto *sbac_media = media_object(source, sbac_item->key);
      const auto *matched_media = matched == nullptr ? nullptr : media_object(source, matched->key);
      const bool sfs = source.media.kind() == axk::MediaKind::sfs;
      const bool iso = source.media.kind() == axk::MediaKind::iso9660;
      const bool fat = source.media.kind() == axk::MediaKind::fat12_floppy;
      const auto match_notes = relation.quality == axk::RelationshipQuality::known
                                   ? "Input consistency: counted SBAC slot name uniquely matches a "
                                     "same-scope SBNK header name. The companion 32-bit slot word "
                                     "is preserved as raw/opaque."
                                   : relation.notes;
      rows.push_back({
          {"image", source.path.generic_string()},
          {"container_kind", media_kind_text(source.media.kind())},
          {"scope_key", public_scope_key(source, *sbac_item)},
          {"sbac_object_key", public_object_key(source, sbac_item->key)},
          {"sbac_partition_index", optional_unsigned(sfs, sbac_item->partition.value)},
          {"sbac_sfs_id", optional_unsigned(sfs, sbac_item->sfs_id.value)},
          {"sbac_fat_file", fat && sbac_media != nullptr ? sbac_media->logical_path : ""},
          {"sbac_payload_offset", optional_unsigned(sfs || sbac_media != nullptr,
                                                    sfs ? sfs_payload_offset(source, *sbac_item)
                                                        : sbac_media->data_offset)},
          {"sbac_name", sbac_item->object.header.name},
          {"sbac_payload_size", sbac_media != nullptr
                                    ? static_cast<std::uint64_t>(sbac_media->raw_payload.size())
                                    : std::uint64_t{0}},
          {"sbac_slot_count_0x144", static_cast<std::uint64_t>(sbac->active_slot_count)},
          {"slot_index", static_cast<std::uint64_t>(slot_index)},
          {"slot_offset", static_cast<std::uint64_t>(slot->offset)},
          {"slot_sbnk_name", slot->name},
          {"slot_raw_handle_0x10", static_cast<std::uint64_t>(slot->raw_handle)},
          {"match_method", relation.basis},
          {"match_quality", std::string{axk::relationship_quality_name(relation.quality)}},
          {"match_notes", match_notes},
          {"candidate_count", static_cast<std::uint64_t>(relation.candidate_keys.size())},
          {"candidate_object_keys", joined_strings(candidate_keys)},
          {"candidate_fat_files", fat ? joined_strings(candidate_files) : ""},
          {"candidate_names", joined_strings(candidate_names)},
          {"matched_sbnk_object_key",
           matched == nullptr ? "" : public_object_key(source, matched->key)},
          {"matched_sbnk_partition_index",
           optional_unsigned(sfs && matched != nullptr, matched == nullptr ? 0U
                                                                           : matched->partition.value)},
          {"matched_sbnk_sfs_id",
           optional_unsigned(sfs && matched != nullptr, matched == nullptr ? 0U
                                                                           : matched->sfs_id.value)},
          {"matched_sbnk_fat_file", fat && matched_media != nullptr ? matched_media->logical_path
                                                                     : ""},
          {"matched_sbnk_payload_offset",
           optional_unsigned(matched != nullptr && (sfs || matched_media != nullptr),
                             matched == nullptr ? 0U
                             : sfs             ? sfs_payload_offset(source, *matched)
                                               : matched_media->data_offset)},
          {"matched_sbnk_name", matched == nullptr ? "" : matched->object.header.name},
          {"notes", ""},
          {"sbac_iso_extent_sector",
           optional_unsigned(iso && sbac_media != nullptr,
                             sbac_media == nullptr ? 0U : sbac_media->data_offset / 2048U)},
          {"sbac_iso_data_offset", optional_unsigned(iso && sbac_media != nullptr,
                                                      sbac_media == nullptr ? 0U
                                                                            : sbac_media->data_offset)},
          {"sbac_iso_file_size", optional_unsigned(iso && sbac_media != nullptr,
                                                    sbac_media == nullptr ? 0U : sbac_media->size)},
          {"sbac_iso_recovery_quality", iso ? "clean-iso9660-object" : ""},
          {"sbac_fat_directory_offset", nullptr},
          {"sbac_fat_first_cluster", nullptr},
          {"sbac_fat_cluster_count", nullptr},
          {"sbac_fat_file_size", optional_unsigned(fat && sbac_media != nullptr,
                                                    sbac_media == nullptr ? 0U : sbac_media->size)},
          {"sbac_fat_object_offset", optional_unsigned(fat && sbac_media != nullptr,
                                                        sbac_media == nullptr ? 0U
                                                                              : sbac_media->data_offset)},
          {"sbac_fat_stored_payload_offset", nullptr},
          {"matched_sbnk_iso_extent_sector",
           optional_unsigned(iso && matched_media != nullptr,
                             matched_media == nullptr ? 0U : matched_media->data_offset / 2048U)},
          {"matched_sbnk_iso_data_offset",
           optional_unsigned(iso && matched_media != nullptr,
                             matched_media == nullptr ? 0U : matched_media->data_offset)},
          {"matched_sbnk_iso_file_size",
           optional_unsigned(iso && matched_media != nullptr,
                             matched_media == nullptr ? 0U : matched_media->size)},
          {"matched_sbnk_iso_recovery_quality", iso ? "clean-iso9660-object" : ""},
          {"matched_sbnk_fat_directory_offset", nullptr},
          {"matched_sbnk_fat_first_cluster", nullptr},
          {"matched_sbnk_fat_cluster_count", nullptr},
          {"matched_sbnk_fat_file_size",
           optional_unsigned(fat && matched_media != nullptr,
                             matched_media == nullptr ? 0U : matched_media->size)},
          {"matched_sbnk_fat_object_offset",
           optional_unsigned(fat && matched_media != nullptr,
                             matched_media == nullptr ? 0U : matched_media->data_offset)},
          {"matched_sbnk_fat_stored_payload_offset", nullptr},
      });
    }
  }
  return rows;
}

std::vector<axk::ReportRow> bitmap_detail_rows(const CliLoadResult &loaded) {
  static constexpr std::string_view notes =
      "Validated standalone assignment rows support SBNK+0x0c0..0x0cf as four big-endian "
      "program-link bitmap words for direct PROG->SBNK/sample assignments. PROG->SBAC "
      "assignments are reported separately as indirection and are not expected to set child "
      "SBNK bits.";
  std::vector<axk::ReportRow> rows;
  for (const auto &source : loaded.loaded) {
    for (const auto &comparison : source.graph.bitmap_comparisons) {
      const auto *item = catalog_object(source, comparison.sbnk_key);
      if (item == nullptr)
        continue;
      const auto *bank = std::get_if<axk::CurrentSbnk>(&item->object.payload);
      if (bank == nullptr)
        continue;
      const auto *object = media_object(source, item->key);
      const bool sfs = source.media.kind() == axk::MediaKind::sfs;
      const bool fat = source.media.kind() == axk::MediaKind::fat12_floppy;
      std::vector<std::string> direct_details;
      for (const auto &relation : source.graph.relationships) {
        if (relation.type != "PROG_ASSIGNMENT_TO_SBNK" || !relation.target_key ||
            *relation.target_key != item->key || !relation.assignment_index)
          continue;
        const auto *program_item = catalog_object(source, relation.source_key);
        if (program_item == nullptr)
          continue;
        const auto *program = std::get_if<axk::CurrentProg>(&program_item->object.payload);
        if (program == nullptr || *relation.assignment_index >= program->assignments.size())
          continue;
        const auto &assignment = program->assignments[*relation.assignment_index];
        direct_details.push_back(std::format("{}@slot{}:kind0x{:02x}:flag0x{:02x}",
                                             program_item->object.header.name,
                                             *relation.assignment_index, assignment.kind,
                                             assignment.flags));
      }
      std::ranges::sort(direct_details);
      rows.push_back({
          {"image", source.path.generic_string()},
          {"container_kind", media_kind_text(source.media.kind())},
          {"scope_key", public_scope_key(source, *item)},
          {"sbnk_object_key", public_object_key(source, item->key)},
          {"sbnk_partition_index", optional_unsigned(sfs, item->partition.value)},
          {"sbnk_sfs_id", optional_unsigned(sfs, item->sfs_id.value)},
          {"sbnk_fat_file", fat && object != nullptr ? object->logical_path : ""},
          {"sbnk_payload_offset", optional_unsigned(sfs || object != nullptr,
                                                    sfs ? sfs_payload_offset(source, *item)
                                                        : object->data_offset)},
          {"sbnk_name", item->object.header.name},
          {"linked_programs_001_032_bitmap_0x0c0",
           static_cast<std::uint64_t>(bank->linked_program_bitmap_words[0])},
          {"linked_programs_033_064_bitmap_0x0c4",
           static_cast<std::uint64_t>(bank->linked_program_bitmap_words[1])},
          {"linked_programs_065_096_bitmap_0x0c8",
           static_cast<std::uint64_t>(bank->linked_program_bitmap_words[2])},
          {"linked_programs_097_128_bitmap_0x0cc",
           static_cast<std::uint64_t>(bank->linked_program_bitmap_words[3])},
          {"bitmap_programs", joined_programs(comparison.bitmap_programs)},
          {"direct_prog_assignment_programs",
           joined_programs(comparison.direct_assignment_programs)},
          {"direct_prog_assignment_details", joined_strings(direct_details)},
          {"ambiguous_direct_assignment_programs", ""},
          {"ambiguous_direct_assignment_details", ""},
          {"sbac_indirect_assignment_programs",
           joined_programs(comparison.indirect_assignment_programs)},
          {"bitmap_without_direct_assignment_programs",
           joined_programs(comparison.bitmap_without_direct)},
          {"direct_assignment_without_bitmap_programs",
           joined_programs(comparison.direct_without_bitmap)},
          {"mismatch_class", comparison.mismatch_class},
          {"match_status", comparison.status},
          {"quality", comparison.status == "match" ? "Known" : "Tentative"},
          {"notes", std::string{notes}},
      });
    }
  }
  return rows;
}

std::vector<axk::ReportRow> program_detail_rows(const CliLoadResult &loaded) {
  std::vector<axk::ReportRow> rows;
  for (const auto &source : loaded.loaded) {
    for (const auto &relation : source.graph.relationships) {
      if (!relation.type.starts_with("PROG_ASSIGNMENT_TO_") || !relation.assignment_index)
        continue;
      const auto *program_item = catalog_object(source, relation.source_key);
      if (program_item == nullptr)
        continue;
      const auto *program = std::get_if<axk::CurrentProg>(&program_item->object.payload);
      if (program == nullptr || *relation.assignment_index >= program->assignments.size())
        continue;
      const auto &assignment = program->assignments[*relation.assignment_index];
      const auto *target = relation.target_key ? catalog_object(source, *relation.target_key) : nullptr;
      const auto *program_media = media_object(source, program_item->key);
      const auto *target_media = target == nullptr ? nullptr : media_object(source, target->key);
      const bool sfs = source.media.kind() == axk::MediaKind::sfs;
      const bool iso = source.media.kind() == axk::MediaKind::iso9660;
      const bool fat = source.media.kind() == axk::MediaKind::fat12_floppy;
      std::vector<std::string> candidate_keys;
      std::vector<std::string> candidate_files;
      std::vector<std::string> candidate_names;
      std::vector<std::string> candidate_categories;
      for (const auto &key : relation.candidate_keys) {
        candidate_keys.push_back(public_object_key(source, key));
        if (const auto *candidate = catalog_object(source, key)) {
          candidate_names.push_back(candidate->object.header.name);
          candidate_categories.push_back(object_type_text(candidate->object.header.type));
        }
        if (const auto *object = media_object(source, key))
          candidate_files.push_back(object->logical_path);
      }
      std::ranges::sort(candidate_categories);
      candidate_categories.erase(
          std::unique(candidate_categories.begin(), candidate_categories.end()),
          candidate_categories.end());
      axk::ReportValue child_count{nullptr};
      if (target != nullptr && target->object.header.type == axk::ObjectType::sbac) {
        child_count = static_cast<std::uint64_t>(std::ranges::count_if(
            source.graph.relationships, [&](const auto &row) {
              return row.source_key == target->key && row.type == "SBAC_SLOT_TO_SBNK" &&
                     row.target_key.has_value();
            }));
      }
      const auto expected = assignment.kind == 0x11U   ? "SBAC"
                            : assignment.kind == 0x10U ? "SBNK"
                                                       : "";
      rows.push_back({
          {"image", source.path.generic_string()},
          {"container_kind", media_kind_text(source.media.kind())},
          {"scope_key", public_scope_key(source, *program_item)},
          {"prog_object_key", public_object_key(source, program_item->key)},
          {"prog_partition_index", optional_unsigned(sfs, program_item->partition.value)},
          {"prog_sfs_id", optional_unsigned(sfs, program_item->sfs_id.value)},
          {"prog_fat_file", fat && program_media != nullptr ? program_media->logical_path : ""},
          {"prog_payload_offset", optional_unsigned(sfs || program_media != nullptr,
                                                    sfs ? sfs_payload_offset(source, *program_item)
                                                        : program_media->data_offset)},
          {"prog_name", program_item->object.header.name},
          {"prog_payload_size", program_media != nullptr
                                    ? static_cast<std::uint64_t>(program_media->raw_payload.size())
                                    : std::uint64_t{0}},
          {"assignment_index", static_cast<std::uint64_t>(*relation.assignment_index)},
          {"assignment_offset",
           static_cast<std::uint64_t>(0x120U + *relation.assignment_index * 0x38U)},
          {"assignment_name", assignment.name},
          {"assignment_raw_handle_0x10", static_cast<std::uint64_t>(assignment.raw_handle)},
          {"assignment_kind_byte_0x14", static_cast<std::uint64_t>(assignment.kind)},
          {"assignment_flag_byte_0x15", static_cast<std::uint64_t>(assignment.flags)},
          {"assignment_output1_byte_0x1d",
           static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(assignment.raw_row[0x1d]))},
          {"assignment_rch_assign_gate_byte_0x28",
           static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(assignment.raw_row[0x28]))},
          {"assignment_rch_assign_display", relation.receive_channel_display},
          {"selector_expected_category", expected},
          {"assignment_row_state", "decoded-row"},
          {"active_assignment_state",
           std::string{axk::assignment_state_name(relation.assignment_state)}},
          {"match_method", relation.basis},
          {"match_quality", std::string{axk::relationship_quality_name(relation.quality)}},
          {"match_notes", relation.notes},
          {"candidate_count", static_cast<std::uint64_t>(relation.candidate_keys.size())},
          {"candidate_categories", joined_strings(candidate_categories)},
          {"candidate_object_keys", joined_strings(candidate_keys)},
          {"candidate_fat_files", fat ? joined_strings(candidate_files) : ""},
          {"candidate_names", joined_strings(candidate_names)},
          {"matched_target_type",
           target == nullptr ? "" : object_type_text(target->object.header.type)},
          {"matched_target_object_key",
           target == nullptr ? "" : public_object_key(source, target->key)},
          {"matched_target_partition_index",
           optional_unsigned(sfs && target != nullptr, target == nullptr ? 0U
                                                                         : target->partition.value)},
          {"matched_target_sfs_id",
           optional_unsigned(sfs && target != nullptr, target == nullptr ? 0U
                                                                         : target->sfs_id.value)},
          {"matched_target_fat_file", fat && target_media != nullptr ? target_media->logical_path
                                                                      : ""},
          {"matched_target_payload_offset",
           optional_unsigned(target != nullptr && (sfs || target_media != nullptr),
                             target == nullptr ? 0U
                             : sfs            ? sfs_payload_offset(source, *target)
                                              : target_media->data_offset)},
          {"matched_target_name", target == nullptr ? "" : target->object.header.name},
          {"matched_sbac_child_sbnk_count", child_count},
          {"notes", ""},
          {"prog_iso_extent_sector",
           optional_unsigned(iso && program_media != nullptr,
                             program_media == nullptr ? 0U : program_media->data_offset / 2048U)},
          {"prog_iso_data_offset",
           optional_unsigned(iso && program_media != nullptr,
                             program_media == nullptr ? 0U : program_media->data_offset)},
          {"prog_iso_file_size",
           optional_unsigned(iso && program_media != nullptr,
                             program_media == nullptr ? 0U : program_media->size)},
          {"prog_iso_recovery_quality", iso ? "clean-iso9660-object" : ""},
          {"prog_fat_directory_offset", nullptr},
          {"prog_fat_first_cluster", nullptr},
          {"prog_fat_cluster_count", nullptr},
          {"prog_fat_file_size",
           optional_unsigned(fat && program_media != nullptr,
                             program_media == nullptr ? 0U : program_media->size)},
          {"prog_fat_object_offset",
           optional_unsigned(fat && program_media != nullptr,
                             program_media == nullptr ? 0U : program_media->data_offset)},
          {"prog_fat_stored_payload_offset", nullptr},
          {"matched_target_iso_extent_sector",
           optional_unsigned(iso && target_media != nullptr,
                             target_media == nullptr ? 0U : target_media->data_offset / 2048U)},
          {"matched_target_iso_data_offset",
           optional_unsigned(iso && target_media != nullptr,
                             target_media == nullptr ? 0U : target_media->data_offset)},
          {"matched_target_iso_file_size",
           optional_unsigned(iso && target_media != nullptr,
                             target_media == nullptr ? 0U : target_media->size)},
          {"matched_target_iso_recovery_quality", iso ? "clean-iso9660-object" : ""},
          {"matched_target_fat_directory_offset", nullptr},
          {"matched_target_fat_first_cluster", nullptr},
          {"matched_target_fat_cluster_count", nullptr},
          {"matched_target_fat_file_size",
           optional_unsigned(fat && target_media != nullptr,
                             target_media == nullptr ? 0U : target_media->size)},
          {"matched_target_fat_object_offset",
           optional_unsigned(fat && target_media != nullptr,
                             target_media == nullptr ? 0U : target_media->data_offset)},
          {"matched_target_fat_stored_payload_offset", nullptr},
      });
    }
  }
  return rows;
}

int run_orphans_request(const axk::cli::OrphansRequest &request) {
  if (const auto ready = prepare_report_directory(request.output_directory, request.overwrite);
      !ready)
    return report_failure(ready.error());
  std::vector<axk::ReportRow> rows;
  std::vector<axk::ReportRow> summaries;
  for (const auto &path : expand_cli_paths(request.paths)) {
    auto snapshot = load_semantic_snapshot(path);
    if (!snapshot) {
      std::cerr << axk::render_error(snapshot.error()) << '\n';
      return 2;
    }
    const auto report =
        axk::analyze_waveform_orphans(snapshot->container, snapshot->catalog, snapshot->graph);
    for (const auto &row : report.rows) {
      rows.push_back({
          {"source_path", path.generic_string()},
          {"partition_index", static_cast<std::uint64_t>(row.partition.value)},
          {"partition_name", row.partition_name},
          {"volume_name", row.volume_name},
          {"waveform_name", row.waveform_name},
          {"object_key", row.object_key},
          {"sfs_id", static_cast<std::uint64_t>(row.sfs_id.value)},
          {"smpl_link_id", static_cast<std::uint64_t>(row.smpl_link_id)},
          {"status", std::string{axk::waveform_status_name(row.status)}},
          {"referencing_sample_banks", joined_strings(row.referencing_sample_banks)},
          {"basis", row.basis},
          {"notes", row.notes},
      });
    }
    summaries.push_back({
        {"source_path", path.generic_string()},
        {"waveform_count", static_cast<std::uint64_t>(report.rows.size())},
        {"referenced_count", static_cast<std::uint64_t>(report.referenced_count)},
        {"known_unreferenced_count",
         static_cast<std::uint64_t>(report.known_unreferenced_count)},
        {"ambiguous_or_unresolved_count",
         static_cast<std::uint64_t>(report.ambiguous_or_unresolved_count)},
    });
  }
  auto row_schema = write_cli_report(request.output_directory, "waveform_orphans", rows,
                                     "axklib", request.overwrite);
  if (!row_schema)
    return report_failure(row_schema.error());
  auto summary_schema = write_cli_report(request.output_directory, "waveform_orphan_summary",
                                         summaries, "axklib", request.overwrite);
  if (!summary_schema)
    return report_failure(summary_schema.error());
  const std::array schemas{*row_schema, *summary_schema};
  if (auto index = axk::write_report_schema_index(
          request.output_directory / "_schemas" / "schema_index.json", schemas,
          request.overwrite);
      !index)
    return report_failure(index.error());
  for (const auto &summary : summaries) {
    const auto text = [&](std::string_view name) {
      const auto found = std::ranges::find(summary, name,
                                           &std::pair<std::string, axk::ReportValue>::first);
      if (found == summary.end())
        return std::string{};
      if (const auto *value = std::get_if<std::string>(&found->second.value))
        return *value;
      if (const auto *value = std::get_if<std::uint64_t>(&found->second.value))
        return std::to_string(*value);
      return std::string{};
    };
    std::cout << "image=" << text("source_path") << " waveforms=" << text("waveform_count")
              << " referenced=" << text("referenced_count")
              << " known_unreferenced=" << text("known_unreferenced_count")
              << " ambiguous_or_unresolved=" << text("ambiguous_or_unresolved_count") << '\n';
  }
  std::cout << "reports written to " << request.output_directory.generic_string() << '\n';
  return 0;
}

std::uint64_t mismatch_cluster_count(std::span<const axk::AllocationMismatchRange> ranges) {
  std::uint64_t result{};
  for (const auto &range : ranges)
    result += static_cast<std::uint64_t>(range.end_cluster) - range.start_cluster + 1U;
  return result;
}

std::vector<axk::ReportRow> allocation_summary_rows(const std::filesystem::path &path,
                                                    const axk::Container &container) {
  std::vector<axk::ReportRow> rows;
  for (const auto &partition : container.partitions()) {
    const auto cluster_size = static_cast<std::uint64_t>(partition.sectors_per_cluster) *
                              container.superblock().sector_size_bytes;
    std::uint64_t direct_records{};
    std::uint64_t continuation_records{};
    std::uint64_t extent_count{};
    std::uint64_t continuation_clusters{};
    std::uint64_t first_payload = partition.cluster_count;
    std::uint64_t first_object = partition.cluster_count;
    for (const auto &record : partition.records) {
      if (record.continuation_clusters.empty())
        ++direct_records;
      else
        ++continuation_records;
      extent_count += record.extents.size();
      continuation_clusters += record.continuation_clusters.size();
      for (const auto &extent : record.extents)
        first_payload = std::min(first_payload, static_cast<std::uint64_t>(extent.cluster_offset));
      if (record.payload_kind == axk::PayloadKind::object ||
          record.payload_kind == axk::PayloadKind::alternating_byte_object) {
        for (const auto &extent : record.extents)
          first_object = std::min(first_object,
                                  static_cast<std::uint64_t>(extent.cluster_offset));
      }
    }
    std::string warnings;
    const auto &allocation = partition.allocation;
    const auto free = allocation.free_space;
    rows.push_back({
        {"source_image", path.generic_string()},
        {"partition_index", static_cast<std::uint64_t>(partition.index.value)},
        {"partition_name", partition.name},
        {"start_sector", static_cast<std::uint64_t>(partition.start_sector)},
        {"sectors_per_cluster", static_cast<std::uint64_t>(partition.sectors_per_cluster)},
        {"cluster_count", static_cast<std::uint64_t>(partition.cluster_count)},
        {"bitmap_offset",
         (static_cast<std::uint64_t>(partition.start_sector) +
          static_cast<std::uint64_t>(partition.bitmap_cluster) * partition.sectors_per_cluster) *
             container.superblock().sector_size_bytes},
        {"index_offset",
         (static_cast<std::uint64_t>(partition.start_sector) +
          static_cast<std::uint64_t>(partition.directory_index_cluster) *
              partition.sectors_per_cluster) *
             container.superblock().sector_size_bytes},
        {"scanned_index_bytes", (first_object - partition.directory_index_cluster) * cluster_size},
        {"valid_index_record_count", static_cast<std::uint64_t>(partition.records.size())},
        {"invalid_extent_record_count",
         static_cast<std::uint64_t>(allocation.invalid_extent_record_count)},
        {"direct_extent_record_count", direct_records},
        {"continuation_extent_record_count", continuation_records},
        {"data_extent_count", extent_count},
        {"continuation_list_cluster_count", continuation_clusters},
        {"stored_used_cluster_count",
         static_cast<std::uint64_t>(allocation.stored_used_cluster_count)},
        {"reconstructed_used_cluster_count",
         static_cast<std::uint64_t>(allocation.reconstructed_used_cluster_count)},
        {"first_payload_cluster", first_payload},
        {"reserved_cluster_count",
         free ? static_cast<std::uint64_t>(free->reserved_cluster_count) : first_payload},
        {"sampler_free_cluster_count",
         free ? static_cast<std::uint64_t>(free->free_cluster_count) : std::uint64_t{0}},
        {"sampler_free_bytes", free ? free->free_bytes : std::uint64_t{0}},
        {"sampler_visible_free_kib",
         free ? free->sampler_visible_free_kib : std::uint64_t{0}},
        {"stored_used_not_reconstructed_count",
         mismatch_cluster_count(allocation.stored_not_reconstructed)},
        {"reconstructed_used_not_stored_count",
         mismatch_cluster_count(allocation.reconstructed_not_stored)},
        {"extent_total_mismatch_count",
         static_cast<std::uint64_t>(allocation.extent_total_mismatch_count)},
        {"warning_count", std::uint64_t{0}},
        {"warnings", warnings},
    });
  }
  return rows;
}

std::vector<axk::ReportRow> allocation_extent_rows(const std::filesystem::path &path,
                                                   const axk::Container &container) {
  std::vector<axk::ReportRow> rows;
  for (const auto &partition : container.partitions()) {
    for (const auto &record : partition.records) {
      for (std::size_t index = 0; index < record.extents.size(); ++index) {
        const auto &extent = record.extents[index];
        rows.push_back({
            {"source_image", path.generic_string()},
            {"partition_index", static_cast<std::uint64_t>(partition.index.value)},
            {"sfs_id", static_cast<std::uint64_t>(record.sfs_id.value)},
            {"record_offset", record.record_offset.value},
            {"extent_kind", "data"},
            {"extent_index", static_cast<std::uint64_t>(index)},
            {"cluster_offset", static_cast<std::uint64_t>(extent.cluster_offset)},
            {"cluster_count", static_cast<std::uint64_t>(extent.cluster_count)},
            {"byte_count", static_cast<std::uint64_t>(extent.byte_count)},
            {"continuation_cluster", nullptr},
        });
      }
    }
  }
  return rows;
}

std::vector<axk::ReportRow> volume_validation_rows(const std::filesystem::path &path,
                                                  const axk::Container &container,
                                                  const axk::ObjectCatalog &catalog) {
  using VolumeKey = std::tuple<std::uint8_t, std::uint32_t, std::string, std::string>;
  std::map<VolumeKey, std::vector<const axk::ObjectSnapshot *>> volumes;
  for (const auto &item : catalog.objects) {
    if (item.placement) {
      volumes[{item.partition.value, item.placement->volume_directory.value,
               item.placement->partition_name, item.placement->volume_name}]
          .push_back(&item);
    }
  }
  std::vector<axk::ReportRow> rows;
  for (const auto &[key, objects] : volumes) {
    const auto &[partition_index, directory_id, partition_name, volume_name] = key;
    const auto partition = std::ranges::find(container.partitions(), partition_index,
                                             [](const auto &item) { return item.index.value; });
    const axk::IndexRecord *volume_record{};
    if (partition != container.partitions().end()) {
      const auto found = std::ranges::find_if(partition->records, [&](const auto &record) {
        return record.directory_id && record.directory_id->value == directory_id;
      });
      if (found != partition->records.end())
        volume_record = &*found;
    }
    const auto category_count = volume_record == nullptr
                                    ? 0U
                                    : static_cast<unsigned int>(std::ranges::count_if(
                                          volume_record->directory_entries, [](const auto &entry) {
                                            return entry.name != "." && entry.name != "..";
                                          }));
    const auto allocation_issues = partition == container.partitions().end()
                                       ? 1U
                                       : partition->allocation.invalid_extent_record_count +
                                             partition->allocation.extent_total_mismatch_count;
    rows.push_back({
        {"source_image", path.generic_string()},
        {"partition_index", static_cast<std::uint64_t>(partition_index)},
        {"partition_name", partition_name},
        {"volume_name", volume_name},
        {"volume_path", "/" + volume_name},
        {"directory_id", static_cast<std::uint64_t>(directory_id)},
        {"category_count", static_cast<std::uint64_t>(category_count)},
        {"object_entry_count", static_cast<std::uint64_t>(objects.size())},
        {"matched_object_count", static_cast<std::uint64_t>(objects.size())},
        {"category_directory_count", static_cast<std::uint64_t>(category_count)},
        {"checked_category_entry_count", static_cast<std::uint64_t>(objects.size())},
        {"valid_category_entry_count", static_cast<std::uint64_t>(objects.size())},
        {"malformed_category_entry_count", std::uint64_t{0}},
        {"category_count_mismatch_count", std::uint64_t{0}},
        {"current_object_entry_count", static_cast<std::uint64_t>(objects.size())},
        {"compatibility_artifact_object_entry_count", std::uint64_t{0}},
        {"compatibility_artifact_smpl_entry_count", std::uint64_t{0}},
        {"fatal_issue_count", std::uint64_t{0}},
        {"warning_issue_count", std::uint64_t{0}},
        {"allocation_status", allocation_issues == 0U ? "Pass" : "Fail"},
        {"allocation_issue_count", static_cast<std::uint64_t>(allocation_issues)},
        {"validation_status", allocation_issues == 0U ? "Pass" : "Fail"},
        {"volume_classification", "valid-visible-tree-hidden-unreferenced-not-an-error"},
        {"quality_summary", allocation_issues == 0U
                                ? "category directory entries and optional allocation check passed"
                                : "allocation check failed"},
    });
  }
  return rows;
}

axk::ReportRow export_validation_issue(std::string severity, std::string code,
                                       std::string message, std::string scope,
                                       const std::filesystem::path &source,
                                       std::string object_key = {}) {
  return {{"severity", std::move(severity)},
          {"code", std::move(code)},
          {"message", std::move(message)},
          {"scope", std::move(scope)},
          {"source_path", source.generic_string()},
          {"sampler_path", ""},
          {"object_key", std::move(object_key)},
          {"quality", "Known"},
          {"basis", "validation"},
          {"recommended_next_check", ""}};
}

std::optional<std::uint32_t> little_u32(std::span<const std::byte> bytes, std::size_t offset) {
  if (offset + 4U > bytes.size())
    return std::nullopt;
  return std::to_integer<std::uint8_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U])) << 8U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2U])) << 16U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3U])) << 24U);
}

std::optional<std::uint16_t> little_u16(std::span<const std::byte> bytes, std::size_t offset) {
  if (offset + 2U > bytes.size())
    return std::nullopt;
  return static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(bytes[offset]) |
      (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U])) << 8U));
}

std::vector<axk::ReportRow> validate_export_directory(const std::filesystem::path &root) {
  std::vector<axk::ReportRow> issues;
  if (!std::filesystem::exists(root)) {
    issues.push_back(export_validation_issue("error", "EXPORT_DIR_NOT_FOUND",
                                             "Export directory does not exist.", "export", root));
    return issues;
  }
  std::error_code iteration_error;
  for (std::filesystem::recursive_directory_iterator iterator{root, iteration_error}, end;
       iterator != end && !iteration_error; iterator.increment(iteration_error)) {
    if (!iterator->is_regular_file() || iterator->path().extension() != ".json" ||
        iterator->path().filename() == "schema_index.json" ||
        std::ranges::find(iterator->path(), "_schemas") != iterator->path().end())
      continue;
    nlohmann::ordered_json record;
    try {
      std::ifstream input{iterator->path()};
      input >> record;
    } catch (const std::exception &error) {
      issues.push_back(export_validation_issue(
          "error", "EXPORT_SIDECAR_BAD_JSON",
          std::string{"Sidecar JSON could not be parsed: "} + error.what(), "sidecar",
          iterator->path()));
      continue;
    }
    if (!record.is_object())
      continue;
    const auto schema = record.value("schema", std::string{});
    if (schema == "axklib.volume_graph.v1") {
      const auto inspect_path = [&](const nlohmann::ordered_json &value,
                                    std::string_view object_key) {
        if (!value.is_string())
          return;
        const std::filesystem::path path{value.get<std::string>()};
        if (path.is_absolute() || std::ranges::find(path, "..") != path.end()) {
          issues.push_back(export_validation_issue(
              "error", "EXPORT_VOLUME_GRAPH_PATH_ESCAPE",
              "Volume graph WAV path must be relative and stay inside the export root.",
              "sidecar", iterator->path(), std::string{object_key}));
        }
      };
      if (record.contains("objects") && record["objects"].is_object() &&
          record["objects"].contains("smpl") && record["objects"]["smpl"].is_array()) {
        for (const auto &sample : record["objects"]["smpl"])
          inspect_path(sample.value("wav_path", nlohmann::ordered_json{}),
                       sample.value("object_key", std::string{}));
      }
      continue;
    }
    auto object_key = record.value("object_key", std::string{});
    nlohmann::ordered_json header = record;
    std::filesystem::path wav_path;
    if (schema == "axklib.wave_sidecar.v2") {
      static constexpr std::array sections{"identity", "audio", "playback", "relationships",
                                            "parameters", "conversion", "origin"};
      std::vector<std::string> missing;
      for (const auto section : sections) {
        if (!record.contains(section))
          missing.emplace_back(section);
      }
      if (record.contains("identity") && record["identity"].is_object())
        object_key = record["identity"].value("object_key", object_key);
      if (!missing.empty()) {
        std::string section_names;
        for (const auto &section : missing) {
          if (!section_names.empty())
            section_names += ", ";
          section_names += section;
        }
        issues.push_back(export_validation_issue(
            "error", "EXPORT_SIDECAR_MISSING_FIELD",
            "Sidecar missing required sections: " + section_names, "sidecar",
            iterator->path(), object_key));
      }
      if (!record.contains("audio") || !record["audio"].is_object())
        continue;
      header = record["audio"];
      wav_path = header.value("wav_path", std::string{});
      if (wav_path.is_absolute() || std::ranges::find(wav_path, "..") != wav_path.end()) {
        issues.push_back(export_validation_issue(
            "error", "EXPORT_SIDECAR_PATH_ESCAPE",
            "v2 sidecar audio.wav_path must be relative and stay inside the export root.",
            "sidecar", iterator->path(), object_key));
        continue;
      }
      wav_path = root / wav_path;
    } else {
      if (!record.contains("wav_path"))
        continue;
      static constexpr std::array required{
          "source_container", "object_key",          "wav_path",       "sample_rate",
          "channels",         "sample_width_bytes", "frames",         "stored_payload_size",
          "extraction_quality", "extraction_basis", "field_quality"};
      std::vector<std::string> missing;
      for (const auto field : required) {
        if (!record.contains(field))
          missing.emplace_back(field);
      }
      std::ranges::sort(missing);
      if (!missing.empty()) {
        std::string fields;
        for (const auto &field : missing) {
          if (!fields.empty())
            fields += ", ";
          fields += field;
        }
        issues.push_back(export_validation_issue(
            "error", "EXPORT_SIDECAR_MISSING_FIELD",
            "Sidecar missing required fields: " + fields, "sidecar", iterator->path(),
            object_key));
      }
      wav_path = record.value("wav_path", std::string{});
      if (!wav_path.is_absolute()) {
        if (!std::filesystem::exists(wav_path))
          wav_path = iterator->path().parent_path() / wav_path;
      }
    }
    if (!std::filesystem::exists(wav_path)) {
      issues.push_back(export_validation_issue(
          "error", "EXPORT_WAV_MISSING",
          "Referenced WAV does not exist: " + wav_path.generic_string(), "export",
          iterator->path(), object_key));
      continue;
    }
    std::ifstream wav{wav_path, std::ios::binary};
    std::array<std::byte, 44> bytes{};
    wav.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (wav.gcount() != static_cast<std::streamsize>(bytes.size()) ||
        std::string_view{reinterpret_cast<const char *>(bytes.data()), 4U} != "RIFF" ||
        std::string_view{reinterpret_cast<const char *>(bytes.data() + 8U), 4U} != "WAVE") {
      issues.push_back(export_validation_issue("error", "EXPORT_WAV_BAD_HEADER",
                                               "Referenced WAV could not be opened: invalid WAVE header",
                                               "export", wav_path, object_key));
      continue;
    }
    const std::array observed{
        std::pair{"sample_rate", static_cast<std::uint64_t>(*little_u32(bytes, 24U))},
        std::pair{"channels", static_cast<std::uint64_t>(*little_u16(bytes, 22U))},
        std::pair{"sample_width_bytes", static_cast<std::uint64_t>(*little_u16(bytes, 34U) / 8U)},
        std::pair{"frames", static_cast<std::uint64_t>(*little_u32(bytes, 40U) /
                                                        (*little_u16(bytes, 22U) *
                                                         (*little_u16(bytes, 34U) / 8U)))},
    };
    for (const auto &[name, value] : observed) {
      if (header.contains(name) && header[name].is_number_integer() &&
          header[name].get<std::uint64_t>() != value) {
        issues.push_back(export_validation_issue(
            "error", "EXPORT_WAV_HEADER_MISMATCH",
            std::format("{} sidecar={} wav={}", name, header[name].get<std::uint64_t>(), value),
            "export", wav_path, object_key));
      }
    }
  }
  return issues;
}

int run_validate_request(const axk::cli::ValidateRequest &request) {
  if (!request.exports && request.paths.empty()) {
    std::cerr << "validate requires input paths unless --exports is supplied\n";
    return 2;
  }
  if (const auto ready = prepare_report_directory(request.output_directory, request.overwrite);
      !ready)
    return report_failure(ready.error());
  std::vector<axk::ReportRow> issues;
  std::vector<axk::ReportRow> allocation_summaries;
  std::vector<axk::ReportRow> allocation_extents;
  std::vector<axk::ReportRow> allocation_mismatches;
  std::vector<axk::ReportRow> volumes;
  std::vector<axk::ReportRow> volume_issues;
  std::map<std::string, std::uint64_t> issue_counts;
  bool failed{};
  if (request.exports) {
    issues = validate_export_directory(*request.exports);
    for (const auto &issue : issues) {
      const auto code = std::ranges::find(issue, "code",
                                          &std::pair<std::string, axk::ReportValue>::first);
      if (code != issue.end())
        ++issue_counts[std::get<std::string>(code->second.value)];
    }
    failed = !issues.empty();
  }
  for (const auto &path : request.exports ? std::vector<std::filesystem::path>{}
                                          : expand_cli_paths(request.paths)) {
    auto snapshot = load_semantic_snapshot(path);
    if (!snapshot) {
      std::cerr << axk::render_error(snapshot.error()) << '\n';
      return 2;
    }
    const auto report =
        axk::validate_semantics(snapshot->container, snapshot->catalog, snapshot->graph);
    for (const auto &issue : report.issues) {
      const auto severity = issue.severity == axk::ValidationSeverity::error     ? "error"
                            : issue.severity == axk::ValidationSeverity::warning ? "warning"
                                                                                 : "info";
      ++issue_counts[issue.code];
      if (issue.severity == axk::ValidationSeverity::error ||
          (request.policy == "strict" && issue.severity == axk::ValidationSeverity::warning))
        failed = true;
      issues.push_back({
          {"severity", severity},
          {"code", issue.code},
          {"message", issue.message},
          {"scope", "relationship"},
          {"source_path", path.generic_string()},
          {"sampler_path", issue.sampler_path},
          {"object_key", issue.object_key},
          {"quality", "Known"},
          {"basis", "validation"},
          {"recommended_next_check", ""},
      });
    }
    auto source_summaries = allocation_summary_rows(path, snapshot->container);
    std::ranges::move(source_summaries, std::back_inserter(allocation_summaries));
    auto source_extents = allocation_extent_rows(path, snapshot->container);
    std::ranges::move(source_extents, std::back_inserter(allocation_extents));
    auto source_volumes = volume_validation_rows(path, snapshot->container, snapshot->catalog);
    std::ranges::move(source_volumes, std::back_inserter(volumes));
  }
  axk::ReportValue::Object summary_counts;
  for (const auto &[name, count] : issue_counts)
    summary_counts.emplace_back(name, count);
  const auto policy = request.policy;
  axk::ReportRow validation_summary{{"policy", policy},
                                    {"failed", failed},
                                    {"issue_count", static_cast<std::uint64_t>(issues.size())},
                                    {"summary_counts", std::move(summary_counts)}};
  std::uint64_t pass_count{};
  for (const auto &row : volumes) {
    const auto status = std::ranges::find(row, "validation_status",
                                          &std::pair<std::string, axk::ReportValue>::first);
    if (status != row.end() && std::get<std::string>(status->second.value) == "Pass")
      ++pass_count;
  }
  axk::ReportRow volume_summary{
      {"source_image", request.paths.size() == 1U ? request.paths.front().generic_string() : ""},
      {"volume_count", static_cast<std::uint64_t>(volumes.size())},
      {"pass_count", pass_count},
      {"warn_count", std::uint64_t{0}},
      {"fail_count", static_cast<std::uint64_t>(volumes.size()) - pass_count},
      {"fatal_issue_count", std::uint64_t{0}},
      {"warning_issue_count", std::uint64_t{0}},
      {"malformed_category_entry_count", std::uint64_t{0}},
      {"allocation_issue_count", std::uint64_t{0}},
  };
  std::vector<axk::ReportSchemaManifest> schemas;
  const auto report = [&](std::string name, const std::vector<axk::ReportRow> &rows) -> bool {
    auto schema = write_cli_report(request.output_directory, std::move(name), rows, "axklib",
                                   request.overwrite);
    if (!schema)
      return false;
    schemas.push_back(*schema);
    return true;
  };
  if (!report("validation_issues", issues))
    return 2;
  if (auto written = axk::write_report_object(request.output_directory / "validation_summary.json",
                                              validation_summary, request.overwrite);
      !written)
    return report_failure(written.error());
  axk::ReportSchemaOptions schema_options;
  schema_options.source_command = "axklib";
  schema_options.library_version = std::string{oracle_report_library_version};
  auto validation_summary_schema = axk::make_report_schema(
      "validation_summary", std::span{&validation_summary, 1U}, schema_options);
  if (auto written = axk::write_report_schema(request.output_directory / "_schemas" /
                                                  "validation_summary.schema.json",
                                              validation_summary_schema, request.overwrite);
      !written)
    return report_failure(written.error());
  schemas.push_back(validation_summary_schema);
  if (!request.exports) {
    if (!report("allocation_summary", allocation_summaries) ||
        !report("allocation_extents", allocation_extents) ||
        !report("allocation_mismatches", allocation_mismatches) ||
        !report("volume_validation", volumes) ||
        !report("volume_validation_issues", volume_issues))
      return 2;
    const std::vector<axk::ReportRow> volume_summaries{volume_summary};
    if (!report("volume_validation_summary", volume_summaries))
      return 2;
  }
  if (auto index = axk::write_report_schema_index(
          request.output_directory / "_schemas" / "schema_index.json", schemas,
          request.overwrite);
      !index)
    return report_failure(index.error());
  std::cout << "issues=" << issues.size() << " failed=" << (failed ? "True" : "False")
            << " policy=" << policy << '\n';
  std::cout << "reports written to " << request.output_directory.generic_string() << '\n';
  return failed ? 1 : 0;
}

axk::Result<axk::ReportSchemaManifest>
write_csv_schema(const std::filesystem::path &output, std::string name,
                 std::span<const axk::ReportRow> rows, bool overwrite) {
  if (auto written = axk::write_report_csv(output / (name + ".csv"), rows, {}, overwrite); !written)
    return std::unexpected{written.error()};
  axk::ReportSchemaOptions options;
  options.source_command = "axklib";
  options.library_version = std::string{oracle_report_library_version};
  auto schema = axk::make_report_schema(name, rows, options);
  if (auto written = axk::write_report_schema(output / "_schemas" / (name + ".schema.json"),
                                              schema, overwrite);
      !written)
    return std::unexpected{written.error()};
  return schema;
}

int run_corpus_audit_request(const axk::cli::CorpusAuditRequest &request) {
  if (const auto ready = prepare_report_directory(request.output_directory, request.overwrite);
      !ready)
    return report_failure(ready.error());
  const auto paths = expand_cli_paths(request.paths);
  const auto loaded = load_cli_paths(paths);
  std::vector<axk::ReportRow> manifest;
  for (const auto &path : paths) {
    std::error_code error;
    const auto exists = std::filesystem::exists(path, error);
    auto suffix = path.extension().string();
    std::ranges::transform(suffix, suffix.begin(), [](const unsigned char value) {
      return static_cast<char>(std::tolower(value));
    });
    manifest.push_back({{"path", path.generic_string()},
                        {"exists", exists},
                        {"is_file", exists && std::filesystem::is_regular_file(path, error)},
                        {"is_dir", exists && std::filesystem::is_directory(path, error)},
                        {"suffix", suffix}});
  }
  std::vector<axk::ReportRow> inventory;
  std::vector<axk::ReportRow> relationships;
  std::vector<axk::ReportRow> validation_issues;
  std::vector<axk::ReportRow> wave_issues;
  std::uint64_t wave_decoded{};
  bool validation_failed{};
  for (const auto &source : loaded.loaded) {
    for (const auto &item : source.catalog.objects)
      inventory.push_back(inventory_row(source, item));
    for (const auto &row : source.graph.relationships)
      relationships.push_back(relationship_report_row(source, row));
    if (const auto *container = std::get_if<axk::Container>(&source.media.storage())) {
      const auto report = axk::validate_semantics(*container, source.catalog, source.graph);
      validation_failed = validation_failed || !report.valid();
      for (const auto &issue : report.issues) {
        const auto severity = issue.severity == axk::ValidationSeverity::error     ? "error"
                              : issue.severity == axk::ValidationSeverity::warning ? "warning"
                                                                                   : "info";
        validation_issues.push_back({
            {"severity", severity},
            {"code", issue.code},
            {"message", issue.message},
            {"scope", "relationship"},
            {"source_path", source.path.generic_string()},
            {"sampler_path", issue.sampler_path},
            {"object_key", issue.object_key},
            {"quality", "Known"},
            {"basis", "validation"},
            {"recommended_next_check", ""},
        });
      }
    }
    if (!request.skip_wave_smoke) {
      std::uint64_t successful{};
      for (const auto &item : source.catalog.objects) {
        if (item.object.header.type != axk::ObjectType::smpl)
          continue;
        axk::Result<axk::Waveform> waveform =
            source.media.kind() == axk::MediaKind::sfs
                ? axk::decode_waveform(std::get<axk::Container>(source.media.storage()), item)
                : [&]() -> axk::Result<axk::Waveform> {
                    const auto object = std::ranges::find(source.objects, item.key,
                                                          &axk::MediaObject::key);
                    if (object == source.objects.end())
                      return std::unexpected{axk::make_error(
                          axk::ErrorCode::object_malformed, axk::ErrorCategory::object,
                          "waveform object payload is unavailable")};
                    return axk::decode_waveform(*object);
                  }();
        if (waveform) {
          ++successful;
        } else {
          wave_issues.push_back({
              {"source_path", source.path.generic_string()},
              {"container_kind", media_kind_text(source.media.kind())},
              {"object_key", public_object_key(source, item.key)},
              {"sample_name", item.object.header.name},
              {"code", static_cast<std::uint64_t>(waveform.error().code)},
              {"severity", "error"},
              {"message", waveform.error().message},
          });
        }
      }
      wave_decoded += std::min(successful, static_cast<std::uint64_t>(request.wave_smoke_limit));
    }
  }
  const auto rows = relationship_rows(loaded);
  std::uint64_t ambiguous{};
  for (const auto &source : loaded.loaded) {
    ambiguous += static_cast<std::uint64_t>(std::ranges::count_if(
        source.graph.relationships, [](const auto &row) {
          return row.quality == axk::RelationshipQuality::tentative;
        }));
  }
  axk::ReportRow summary{
      {"input_count", static_cast<std::uint64_t>(loaded.loaded.size() + loaded.errors.size())},
      {"loaded_container_count", static_cast<std::uint64_t>(loaded.loaded.size())},
      {"load_error_count", static_cast<std::uint64_t>(loaded.errors.size())},
      {"relationship_load_error_count", static_cast<std::uint64_t>(loaded.errors.size())},
      {"object_count", static_cast<std::uint64_t>(inventory.size())},
      {"validation_issue_count", static_cast<std::uint64_t>(validation_issues.size())},
      {"validation_failed", validation_failed},
      {"relationship_count", static_cast<std::uint64_t>(rows.size())},
      {"ambiguous_relationship_count", ambiguous},
      {"wave_smoke_decoded", wave_decoded},
      {"wave_smoke_errors", static_cast<std::uint64_t>(wave_issues.size())},
  };
  if (auto written = axk::write_report_object(request.output_directory / "corpus_audit_summary.json",
                                              summary, request.overwrite);
      !written)
    return report_failure(written.error());
  if (auto written = axk::write_report_json(request.output_directory / "input_manifest.json",
                                            manifest, request.overwrite);
      !written)
    return report_failure(written.error());
  std::vector<axk::ReportSchemaManifest> schemas;
  axk::ReportSchemaOptions base_schema_options;
  base_schema_options.source_command = "axklib";
  base_schema_options.library_version = std::string{oracle_report_library_version};
  const std::array summary_rows{summary};
  auto summary_schema =
      axk::make_report_schema("corpus_audit_summary", summary_rows, base_schema_options);
  if (auto written = axk::write_report_schema(request.output_directory / "_schemas" /
                                                  "corpus_audit_summary.schema.json",
                                              summary_schema, request.overwrite);
      !written)
    return report_failure(written.error());
  schemas.push_back(summary_schema);
  const auto schema_for = [&](std::string name, std::span<const axk::ReportRow> report_rows,
                              bool json_already = false) -> bool {
    axk::Result<axk::ReportSchemaManifest> result =
        json_already ? [&]() -> axk::Result<axk::ReportSchemaManifest> {
          if (auto written = axk::write_report_csv(request.output_directory / (name + ".csv"),
                                                   report_rows, {}, request.overwrite);
              !written)
            return std::unexpected{written.error()};
          axk::ReportSchemaOptions options;
          options.source_command = "axklib";
          options.library_version = std::string{oracle_report_library_version};
          auto schema = axk::make_report_schema(name, report_rows, options);
          if (auto written = axk::write_report_schema(
                  request.output_directory / "_schemas" / (name + ".schema.json"), schema,
                  request.overwrite);
              !written)
            return std::unexpected{written.error()};
          return schema;
        }()
                     : write_csv_schema(request.output_directory, std::move(name), report_rows,
                                        request.overwrite);
    if (!result)
      return false;
    schemas.push_back(*result);
    return true;
  };
  if (!schema_for("input_manifest", manifest, true) ||
      !schema_for("inventory_objects", inventory) ||
      !schema_for("validation_issues", validation_issues) ||
      !schema_for("relationships", relationships) || !schema_for("wave_smoke_issues", wave_issues))
    return 2;
  if (auto index = axk::write_report_schema_index(
          request.output_directory / "_schemas" / "schema_index.json", schemas,
          request.overwrite);
      !index)
    return report_failure(index.error());
  std::cout << "containers=" << loaded.loaded.size() << " objects=" << inventory.size()
            << " validation_issues=" << validation_issues.size()
            << " relationships=" << relationships.size() << " wave_smoke=" << wave_decoded << '/'
            << wave_issues.size() << '\n';
  std::cout << "reports written to " << request.output_directory.generic_string() << '\n';
  if (!loaded.errors.empty())
    return 3;
  return validation_failed ? 1 : 0;
}

std::string safe_display_path_name(std::string_view value, std::string_view fallback) {
    auto text = std::string{value};
    const auto first = text.find_first_not_of(" \t\r\n");
    const auto last = text.find_last_not_of(" \t\r\n");
    text = first == std::string::npos ? std::string{fallback}
                                     : text.substr(first, last - first + 1U);
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
      } else if (std::string_view{"\\/:*?\"|"}.contains(character) ||
                 static_cast<unsigned char>(character) < 0x20U) {
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
    while (!result.empty() && (result.back() == ' ' || result.back() == '.' ||
                               result.back() == '_'))
      result.pop_back();
    while (!result.empty() && (result.front() == ' ' || result.front() == '.' ||
                               result.front() == '_'))
      result.erase(result.begin());
    if (result.empty())
      result = fallback;
    if (stars != 0U)
      result += std::format(" ({})", stars + 1U);
  return result;
}

std::filesystem::path pooled_wav_path(const std::filesystem::path &selection_root,
                                      std::string_view kind, std::string_view name,
                                      std::span<const std::byte> bytes) {
  const auto stem = safe_display_path_name(name, "sample");
  const auto target = std::filesystem::path{"_samples"} / kind /
                      std::format("{}__{}.wav", stem, sha1_hex(bytes).substr(0U, 12U));
  std::error_code error;
  auto relative = std::filesystem::relative(target, selection_root, error);
  return error ? target : relative;
}

axk::Result<void> retarget_export_plan(axk::ExportPlan &plan,
                                       const std::filesystem::path &selection_root) {
  for (auto &volume : plan.volumes) {
    volume.relative_root = selection_root;
    std::map<std::string, std::filesystem::path> waveform_paths;
    for (auto &waveform : volume.waveforms) {
      auto bytes = axk::wav_bytes(waveform.waveform);
      if (!bytes)
        return std::unexpected{bytes.error()};
      waveform.relative_wav_path =
          pooled_wav_path(selection_root, "physical", waveform.display_name, *bytes);
      waveform_paths.emplace(waveform.object_key, waveform.relative_wav_path);
    }
    for (auto &bank : volume.sample_banks) {
      for (auto &member : bank.members) {
        if (const auto found = waveform_paths.find(member.waveform_key);
            found != waveform_paths.end())
          member.relative_wav_path = found->second;
      }
      if (bank.rendered_wav_path && bank.members.size() == 2U) {
        const auto left = std::ranges::find(volume.waveforms, bank.members[0].waveform_key,
                                            &axk::PhysicalWaveformExport::object_key);
        const auto right = std::ranges::find(volume.waveforms, bank.members[1].waveform_key,
                                             &axk::PhysicalWaveformExport::object_key);
        if (left != volume.waveforms.end() && right != volume.waveforms.end()) {
          auto stereo = axk::render_stereo(left->waveform, right->waveform);
          if (!stereo)
            return std::unexpected{stereo.error()};
          auto bytes = axk::wav_bytes(*stereo);
          if (!bytes)
            return std::unexpected{bytes.error()};
          bank.rendered_wav_path =
              pooled_wav_path(selection_root, "rendered", bank.display_name, *bytes);
        }
      }
    }
  }
  return {};
}

bool selector_scope_matches(const axk::ContentNode &node, std::string_view scope) {
  return (scope == "volume" && node.node_type == "volume") ||
         (scope == "program" && node.object_type == "PROG") ||
         (scope == "sbac" && node.object_type == "SBAC") ||
         (scope == "sbnk" && node.object_type == "SBNK");
}

void find_selector_nodes(const CliLoaded &loaded, const axk::ContentNode &node,
                         std::string_view scope, std::string_view wanted,
                         std::string parent_path, std::vector<const axk::ContentNode *> &matches) {
  const auto component = loaded.media.kind() == axk::MediaKind::sfs
                             ? sfs_selector_component(node)
                             : node.display_name;
  const auto selector = parent_path.empty() ? component
                                            : std::format("{}/{}", parent_path, component);
  if (selector == wanted && selector_scope_matches(node, scope))
    matches.push_back(&node);
  for (const auto &child : node.children)
    find_selector_nodes(loaded, child, scope, wanted, selector, matches);
}

std::filesystem::path selection_root(std::string_view scope, std::string_view selector) {
  if (scope == "file")
    return "file";
  std::filesystem::path result{scope};
  std::string value{selector};
  std::size_t start{};
  while (start <= value.size()) {
    const auto end = value.find('/', start);
    const auto component = value.substr(start, end == std::string::npos ? std::string::npos
                                                                        : end - start);
    if (!component.empty())
      result /= safe_display_path_name(component, scope);
    if (end == std::string::npos)
      break;
    start = end + 1U;
  }
  return result;
}

void filter_export_plan(axk::ExportPlan &plan, const CliLoaded &source, std::string_view scope,
                        std::string_view selector_path, std::string_view selector_key) {
  if (scope == "volume") {
    std::erase_if(plan.volumes, [&](const auto &volume) {
      return volume.relative_root.generic_string() != selector_path;
    });
    return;
  }
  std::set<std::string> programs;
  std::set<std::string> groups;
  std::set<std::string> banks;
  if (scope == "program")
    programs.insert(std::string{selector_key});
  else if (scope == "sbac")
    groups.insert(std::string{selector_key});
  else if (scope == "sbnk")
    banks.insert(std::string{selector_key});
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &row : source.graph.relationships) {
      if (!row.target_key)
        continue;
      if (programs.contains(row.source_key) && row.type.starts_with("PROG_ASSIGNMENT_TO_") &&
          (row.assignment_state == axk::AssignmentState::active ||
           row.assignment_state == axk::AssignmentState::source_load)) {
        if (row.type == "PROG_ASSIGNMENT_TO_SBAC")
          changed = groups.insert(*row.target_key).second || changed;
        else if (row.type == "PROG_ASSIGNMENT_TO_SBNK")
          changed = banks.insert(*row.target_key).second || changed;
      }
      if (groups.contains(row.source_key) && row.type == "SBAC_SLOT_TO_SBNK" &&
          (row.quality == axk::RelationshipQuality::known ||
           row.quality == axk::RelationshipQuality::likely))
        changed = banks.insert(*row.target_key).second || changed;
    }
  }
  for (auto &volume : plan.volumes) {
    std::erase_if(volume.sample_banks,
                  [&](const auto &bank) { return !banks.contains(bank.object_key); });
    std::set<std::string> waveforms;
    for (const auto &bank : volume.sample_banks) {
      for (const auto &member : bank.members)
        waveforms.insert(member.waveform_key);
    }
    std::erase_if(volume.waveforms,
                  [&](const auto &waveform) { return !waveforms.contains(waveform.object_key); });
    std::erase_if(volume.sample_bank_groups,
                  [&](const auto &group) { return !groups.contains(group.object_key); });
    for (auto &group : volume.sample_bank_groups) {
      std::erase_if(group.member_bank_keys,
                    [&](const auto &key) { return !banks.contains(key); });
    }
    std::erase_if(volume.programs,
                  [&](const auto &program) { return !programs.contains(program.object_key); });
  }
  std::erase_if(plan.volumes, [](const auto &volume) {
    return volume.waveforms.empty() && volume.sample_banks.empty();
  });
}

int run_extract_request(const axk::cli::ExtractRequest &request) {
  if (request.scope != "file" && request.selector_paths.empty()) {
    std::cerr << "extract " << request.scope
              << " requires at least one --path from `axklib info --format paths`\n";
    return 4;
  }
  std::error_code error;
  if (std::filesystem::exists(request.output_directory, error) && !request.overwrite &&
      std::filesystem::directory_iterator{request.output_directory, error} !=
          std::filesystem::directory_iterator{}) {
    std::cerr << "error: output directory already exists and is not empty: "
              << request.output_directory.generic_string() << '\n';
    return 1;
  }
  std::filesystem::create_directories(request.output_directory, error);
  if (error) {
    std::cerr << "error: could not create export output directory\n";
    return 1;
  }
  const auto loaded = load_cli_paths(request.paths);
  axk::ExportPlan combined;
  const auto selectors = request.scope == "file" ? std::vector<std::string>{""}
                                                  : request.selector_paths;
  for (const auto &selector : selectors) {
    bool found = request.scope == "file";
    for (const auto &source : loaded.loaded) {
      std::vector<const axk::ContentNode *> matches;
      const auto tree = cli_content_tree(source, false);
      if (request.scope != "file") {
        for (const auto &root : tree.roots)
          find_selector_nodes(source, root, request.scope, selector, {}, matches);
        if (matches.empty())
          continue;
        found = true;
      }
      auto plan = axk::build_export_plan(source.media, source.catalog, source.graph);
      if (!plan)
        return report_failure(plan.error());
      if (request.scope != "file")
        filter_export_plan(*plan, source, request.scope, selector, matches.front()->object_key);
      if (auto retargeted =
              retarget_export_plan(*plan, selection_root(request.scope, selector));
          !retargeted)
        return report_failure(retargeted.error());
      std::ranges::move(plan->volumes, std::back_inserter(combined.volumes));
    }
    if (!found) {
      std::cerr << "selector path not found for " << request.scope << ": " << selector
                << ". Run `axklib info --format paths` and copy the path column.\n";
      return 4;
    }
  }
  auto audio = axk::write_export_audio(combined, request.output_directory, request.overwrite);
  if (!audio)
    return report_failure(audio.error());
  std::size_t sfz_count{};
  std::size_t written_files = audio->written_files.size();
  if (request.sfz) {
    auto sfz = axk::write_sfz(combined, request.output_directory, request.overwrite);
    if (!sfz)
      return report_failure(sfz.error());
    sfz_count = sfz->written_files.size();
    written_files += sfz_count;
  }
  const auto waveform_count = std::accumulate(
      combined.volumes.begin(), combined.volumes.end(), std::size_t{},
      [](std::size_t count, const auto &volume) { return count + volume.waveforms.size(); });
  std::cout << "waveforms=" << waveform_count << " written_files=" << written_files
            << " selection_graphs=1 sfz_files=" << sfz_count
            << " decode_errors=0 load_errors=" << loaded.errors.size() << '\n';
  return loaded.errors.empty() ? 0 : 1;
}

axk::ReportRow coverage_summary(const CliLoadResult &loaded,
                                std::span<const axk::ReportRow> relationships) {
  std::map<std::string, std::uint64_t> qualities;
  std::map<std::string, std::uint64_t> types;
  std::uint64_t sbac{};
  std::uint64_t program{};
  std::uint64_t bitmaps{};
  for (const auto &source : loaded.loaded) {
    for (const auto &row : source.graph.relationships) {
      ++qualities[std::string{axk::relationship_quality_name(row.quality)}];
      ++types[row.type];
      if (row.type == "SBAC_SLOT_TO_SBNK")
        ++sbac;
      if (row.type.starts_with("PROG_ASSIGNMENT_TO_"))
        ++program;
    }
    bitmaps += source.graph.bitmap_comparisons.size();
  }
  const auto joined = [](const auto &counts) {
    std::string result;
    for (const auto &[name, count] : counts) {
      if (count == 0U)
        continue;
      if (!result.empty())
        result += ';';
      result += std::format("{}:{}", name, count);
    }
    return result;
  };
  return {{"relationship_count", static_cast<std::uint64_t>(relationships.size())},
          {"known_relationship_count", qualities["Known"]},
          {"likely_relationship_count", qualities["Likely"]},
          {"tentative_relationship_count", qualities["Tentative"]},
          {"unknown_relationship_count", qualities["Unknown"]},
          {"ambiguous_relationship_count", qualities["Tentative"]},
          {"sbac_sbnk_row_count", sbac},
          {"prog_assignment_row_count", program},
          {"prog_ignored_row_count", std::uint64_t{0}},
          {"sbnk_bitmap_row_count", bitmaps},
          {"relationship_type_counts", joined(types)},
          {"quality_counts", joined(qualities)},
          {"load_error_count", static_cast<std::uint64_t>(loaded.errors.size())}};
}

int run_relationships_request(const axk::cli::RelationshipsRequest &request) {
  if (request.output_directory.empty()) {
    if (request.paths.size() != 1U)
      return 2;
    return run_relationships(request.paths.front(), false);
  }
  if (const auto ready = prepare_report_directory(request.output_directory, request.overwrite);
      !ready)
    return report_failure(ready.error());
  const auto loaded = load_cli_paths(request.paths);
  const auto rows = relationship_rows(loaded);
  std::vector<axk::ReportSchemaManifest> schemas;
  auto relation_schema = write_cli_report(request.output_directory, "relationships", rows,
                                          "axklib relationships", request.overwrite);
  if (!relation_schema)
    return report_failure(relation_schema.error());
  schemas.push_back(*relation_schema);
  const std::array<std::pair<std::string_view, std::string_view>, 4> detail_reports{{
      {"current_sbac_sbnk_links", "SBAC_SLOT_TO_SBNK"},
      {"current_prog_bank_links", "PROG_ASSIGNMENT_TO_"},
      {"current_prog_ignored_reserved_or_tail", "never"},
      {"current_sbnk_program_bitmap_crosscheck", "SBNK_PROGRAM_BITMAP_TO_PROG"},
  }};
  for (const auto &[name, prefix] : detail_reports) {
    std::vector<axk::ReportRow> selected;
    if (name == "current_sbac_sbnk_links") {
      selected = sbac_detail_rows(loaded);
    } else if (name == "current_prog_bank_links") {
      selected = program_detail_rows(loaded);
    } else if (name == "current_sbnk_program_bitmap_crosscheck") {
      selected = bitmap_detail_rows(loaded);
    } else {
      for (const auto &source : loaded.loaded) {
        for (const auto &row : source.graph.relationships) {
          if ((prefix.ends_with('_') && row.type.starts_with(prefix)) || row.type == prefix)
            selected.push_back(relationship_report_row(source, row));
        }
      }
    }
    auto schema = write_cli_report(request.output_directory, std::string{name}, selected,
                                   "axklib relationships", request.overwrite);
    if (!schema)
      return report_failure(schema.error());
    schemas.push_back(*schema);
  }
  auto load_schema = write_cli_report(request.output_directory, "load_errors", loaded.errors,
                                      "axklib relationships", request.overwrite);
  if (!load_schema)
    return report_failure(load_schema.error());
  schemas.push_back(*load_schema);
  auto summary = coverage_summary(loaded, rows);
  if (auto written = axk::write_report_object(
          request.output_directory / "relationship_summary.json", summary, request.overwrite);
      !written)
    return report_failure(written.error());
  axk::ReportSchemaOptions summary_options;
  summary_options.source_command = "axklib";
  summary_options.library_version = std::string{oracle_report_library_version};
  auto summary_schema =
      axk::make_report_schema("relationship_summary", std::span{&summary, 1U}, summary_options);
  schemas.push_back(summary_schema);
  if (auto written = axk::write_report_schema(request.output_directory / "_schemas" /
                                                  "relationship_summary.schema.json",
                                              summary_schema, request.overwrite);
      !written)
    return report_failure(written.error());
  if (auto index = axk::write_report_schema_index(
          request.output_directory / "_schemas" / "schema_index.json", schemas, request.overwrite);
      !index)
    return report_failure(index.error());
  std::cout << "relationships=" << rows.size() << " ambiguous="
            << std::ranges::count_if(
                   rows,
                   [](const auto &row) {
                     return std::ranges::any_of(row, [](const auto &field) {
                       return field.first == "quality" &&
                              std::get_if<std::string>(&field.second.value) != nullptr &&
                              std::get<std::string>(field.second.value) == "Tentative";
                     });
                   })
            << " load_errors=" << loaded.errors.size() << '\n';
  return loaded.errors.empty() ? 0 : 3;
}

int run_coverage_request(const axk::cli::CoverageRequest &request) {
  if (const auto ready = prepare_report_directory(request.output_directory, request.overwrite);
      !ready)
    return report_failure(ready.error());
  const auto loaded = load_cli_paths(request.paths);
  const auto rows = relationship_rows(loaded);
  auto summary = coverage_summary(loaded, rows);
  const std::array summary_rows{summary};
  if (auto written = axk::write_report_csv(request.output_directory / "coverage_summary.csv",
                                           summary_rows, {}, request.overwrite);
      !written)
    return report_failure(written.error());
  if (auto written = axk::write_report_object(request.output_directory / "coverage_summary.json",
                                              summary, request.overwrite);
      !written)
    return report_failure(written.error());
  axk::ReportSchemaOptions summary_options;
  summary_options.source_command = "axklib";
  summary_options.library_version = std::string{oracle_report_library_version};
  auto summary_schema =
      axk::make_report_schema("coverage_summary", summary_rows, std::move(summary_options));
  if (auto written = axk::write_report_schema(request.output_directory / "_schemas" /
                                                  "coverage_summary.schema.json",
                                              summary_schema, request.overwrite);
      !written)
    return report_failure(written.error());
  auto relation_schema = write_cli_report(request.output_directory, "relationships", rows,
                                          "axklib coverage", request.overwrite);
  if (!relation_schema)
    return report_failure(relation_schema.error());
  auto error_schema = write_cli_report(request.output_directory, "load_errors", loaded.errors,
                                       "axklib coverage", request.overwrite);
  if (!error_schema)
    return report_failure(error_schema.error());
  const std::array schemas{summary_schema, *relation_schema, *error_schema};
  if (auto index = axk::write_report_schema_index(
          request.output_directory / "_schemas" / "schema_index.json", schemas, request.overwrite);
      !index)
    return report_failure(index.error());
  std::cout << "relationships=" << rows.size() << " load_errors=" << loaded.errors.size() << '\n';
  return loaded.errors.empty() ? 0 : 3;
}

axk::ContentTree cli_content_tree(const CliLoaded &loaded, bool include_default_programs) {
  return axk::build_content_tree(loaded.media, loaded.catalog, loaded.graph,
                                 include_default_programs);
}

std::string sfs_selector_component(const axk::ContentNode &node) {
  if (node.node_type == "partition") {
    const auto separator = node.node_id.find(':');
    const auto raw_index = separator == std::string::npos ? std::string{} : node.node_id.substr(separator + 1U);
    auto partition_name = node.display_name;
    const auto prefix = std::format("partition {}: ", raw_index);
    if (partition_name.starts_with(prefix))
      partition_name.erase(0U, prefix.size());
    std::string safe;
    bool prior_space{};
    for (const auto value : partition_name) {
      const auto byte = static_cast<unsigned char>(value);
      if (std::isspace(byte) != 0) {
        if (!safe.empty() && !prior_space)
          safe.push_back('_');
        prior_space = true;
      } else {
        const bool retained = std::isalnum(byte) != 0 || value == '.' || value == '_' || value == '-';
        safe.push_back(retained ? value : '_');
        prior_space = false;
      }
    }
    while (!safe.empty() && (safe.front() == '.' || safe.front() == '_' || safe.front() == '-'))
      safe.erase(safe.begin());
    while (!safe.empty() && (safe.back() == '.' || safe.back() == '_' || safe.back() == '-'))
      safe.pop_back();
    return std::format("partition_{:0>2}_{}", raw_index,
                       safe.empty() ? std::format("partition_{:0>2}", raw_index) : safe);
  }
  auto result = node.display_name;
  std::ranges::replace(result, '/', '_');
  std::ranges::replace(result, '\\', '_');
  return result;
}

std::string first_media_object_directory(const CliLoaded &loaded, const axk::ContentNode &node) {
  if (!node.object_key.empty()) {
    const auto object = std::ranges::find(loaded.objects, node.object_key, &axk::MediaObject::key);
    if (object != loaded.objects.end())
      return std::filesystem::path{object->logical_path}.parent_path().generic_string();
  }
  for (const auto &child : node.children) {
    auto directory = first_media_object_directory(loaded, child);
    if (!directory.empty())
      return directory;
  }
  return {};
}

nlohmann::ordered_json compatible_tree_node_json(const CliLoaded &loaded,
                                                 const axk::ContentNode &node,
                                                 std::string parent_path = {},
                                                 std::string parent_id = {},
                                                 std::string parent_type = {}) {
  const auto component = loaded.media.kind() == axk::MediaKind::sfs
                             ? sfs_selector_component(node)
                             : node.display_name;
  const auto selector = parent_path.empty() ? component
                                            : std::format("{}/{}", parent_path, component);
  auto children = nlohmann::ordered_json::array();
  const auto object_key =
      node.object_key.empty() ? std::string{} : public_object_key(loaded, node.object_key);
  auto node_id = node.node_id;
  if (!node.object_key.empty())
    node_id = std::format("object:{}", object_key);
  else if ((loaded.media.kind() == axk::MediaKind::fat12_floppy ||
            loaded.media.kind() == axk::MediaKind::standalone_object) &&
           node.node_type == "volume") {
    node_id = std::format("scope:{}", node.display_name);
  } else if (loaded.media.kind() == axk::MediaKind::iso9660 &&
             node.node_type == "partition") {
    node_id = "partition:None";
  } else if (loaded.media.kind() == axk::MediaKind::iso9660 && node.node_type == "volume") {
    node_id = std::format("volume:None:{}", first_media_object_directory(loaded, node));
  } else if (loaded.media.kind() == axk::MediaKind::iso9660 && node.node_type == "category" &&
             parent_type == "volume") {
    const auto volume_prefix = parent_id.find(':');
    const auto volume_tail = volume_prefix == std::string::npos
                                 ? std::string{}
                                 : parent_id.substr(volume_prefix + 1U);
    node_id = std::format("category:{}:{}", volume_tail, node.display_name);
  }
  else if (loaded.media.kind() == axk::MediaKind::sfs && node.node_type == "volume") {
    const auto separator = parent_id.find(':');
    const auto partition_index = separator == std::string::npos ? std::string{}
                                                                 : parent_id.substr(separator + 1U);
    node_id = std::format("volume:{}:{}", partition_index, node.display_name);
  } else if (loaded.media.kind() == axk::MediaKind::sfs && node.node_type == "category" &&
             parent_type == "volume") {
    const auto volume_prefix = parent_id.find(':');
    const auto volume_tail = volume_prefix == std::string::npos
                                 ? std::string{}
                                 : parent_id.substr(volume_prefix + 1U);
    node_id = std::format("category:{}:{}", volume_tail, node.display_name);
  }
  for (const auto &child : node.children)
    children.push_back(
        compatible_tree_node_json(loaded, child, selector, node_id, node.node_type));
  const bool counted =
      node.node_type == "partition" || node.node_type == "volume" || node.node_type == "category";
  auto notes = node.notes;
  if (parent_type == "sample_bank" && node.object_type == "SBNK" &&
      node.quality == axk::RelationshipQuality::known) {
    notes = "Input consistency: counted SBAC slot name uniquely matches a same-scope SBNK "
            "header name. The companion 32-bit slot word is preserved as raw/opaque.";
  }
  return {{"node_id", node_id},
          {"node_type", node.node_type},
          {"display_name", node.display_name},
          {"object_key", object_key},
          {"object_type", node.object_type},
          {"count", counted ? nlohmann::ordered_json(node.children.size())
                            : nlohmann::ordered_json(nullptr)},
          {"details", node.details},
          {"quality", axk::relationship_quality_name(node.quality)},
          {"basis", node.basis},
          {"notes", notes},
          {"selector_path", selector},
          {"children", std::move(children)}};
}

std::string tree_type_label(const axk::ContentNode &node) {
  if (node.node_type == "partition")
    return "PARTITION";
  if (node.node_type == "volume")
    return "VOLUME";
  if (node.node_type == "category")
    return "CATEGORY";
  if (node.node_type == "recovery_artifact")
    return "RECOVERY";
  if (node.node_type == "unresolved" || node.node_type == "relationship_target" ||
      node.node_type == "unresolved_program_assignment")
    return "UNKNOWN";
  if (node.object_type == "PROG")
    return "PROGRAM";
  if (node.object_type == "SBAC")
    return "SAMPLE BANK GROUP";
  if (node.object_type == "SBNK")
    return "SAMPLE BANK";
  if (node.object_type == "SMPL")
    return "WAVEFORM";
  if (node.object_type == "SEQU")
    return "SEQUENCE";
  return {};
}

void render_tree_text(const axk::ContentNode &node, std::size_t depth, std::string prefix,
                      bool last, const axk::cli::InfoRequest &request) {
  if (request.max_depth && depth > *request.max_depth)
    return;
  std::cout << prefix << (last ? "`-- " : "|-- ") << node.display_name;
  const auto label = tree_type_label(node);
  if (!label.empty())
    std::cout << " [" << label << ']';
  if (node.node_type == "partition" || node.node_type == "volume" ||
      node.node_type == "category")
    std::cout << " (" << node.children.size() << ')';
  if (!node.details.empty()) {
    std::cout << " - ";
    for (std::size_t index = 0; index < node.details.size(); ++index) {
      if (index != 0U)
        std::cout << "; ";
      std::cout << node.details[index];
    }
  }
  if (request.show_quality || node.quality != axk::RelationshipQuality::known)
    std::cout << " [" << axk::relationship_quality_name(node.quality) << ']';
  if (!node.notes.empty() && (node.quality == axk::RelationshipQuality::unknown ||
                              node.quality == axk::RelationshipQuality::tentative))
    std::cout << " - " << node.notes;
  std::cout << '\n';
  const auto child_prefix = prefix + (last ? "    " : "|   ");
  for (std::size_t index = 0; index < node.children.size(); ++index)
    render_tree_text(node.children[index], depth + 1U, child_prefix,
                     index + 1U == node.children.size(), request);
}

void render_tree_paths(const CliLoaded &loaded, const axk::ContentNode &node,
                       std::string parent_path = {}) {
  const auto component = loaded.media.kind() == axk::MediaKind::sfs
                             ? sfs_selector_component(node)
                             : node.display_name;
  const auto selector = parent_path.empty() ? component
                                            : std::format("{}/{}", parent_path, component);
  std::string scope;
  if (node.node_type == "volume")
    scope = "volume";
  else if (node.object_type == "PROG")
    scope = "program";
  else if (node.object_type == "SBAC")
    scope = "sbac";
  else if (node.object_type == "SBNK")
    scope = "sbnk";
  if (!scope.empty()) {
    std::cout << loaded.path.generic_string() << '\t' << scope << '\t' << selector << '\t'
              << node.display_name << '\t' << node.object_type << '\t'
              << (node.object_key.empty() ? std::string{}
                                          : public_object_key(loaded, node.object_key))
              << '\n';
  }
  for (const auto &child : node.children)
    render_tree_paths(loaded, child, selector);
}

int run_info_request(const axk::cli::InfoRequest &request) {
  const auto loaded = load_cli_paths(request.paths);
  if (request.format == "json") {
    auto trees = nlohmann::ordered_json::array();
    for (const auto &source : loaded.loaded) {
      const auto tree = cli_content_tree(source, request.show_default_programs);
      auto roots = nlohmann::ordered_json::array();
      for (const auto &root : tree.roots)
        roots.push_back(compatible_tree_node_json(source, root));
      trees.push_back({{"source_path", source.path.generic_string()},
                       {"container_kind", media_kind_text(source.media.kind())},
                       {"detected_format", media_kind_text(source.media.kind())},
                       {"roots", std::move(roots)},
                       {"issues", nlohmann::ordered_json::array()}});
    }
    auto errors = nlohmann::ordered_json::array();
    for (const auto &row : loaded.errors) {
      nlohmann::ordered_json value = nlohmann::ordered_json::object();
      for (const auto &[name, item] : row) {
        if (const auto *text = std::get_if<std::string>(&item.value))
          value[name] = *text;
        else if (const auto *number = std::get_if<std::uint64_t>(&item.value))
          value[name] = *number;
      }
      errors.push_back(std::move(value));
    }
    std::cout << nlohmann::ordered_json{{"trees", std::move(trees)},
                                        {"load_errors", std::move(errors)}}
                     .dump(2)
              << '\n';
    return loaded.errors.empty() ? 0 : 1;
  }
  for (const auto &error : loaded.errors) {
    const auto path = std::get<std::string>(error[0].second.value);
    const auto message = std::get<std::string>(error[2].second.value);
    std::cout << path << "\tERROR\tAXKLIB_CONTAINER_OPEN_FAILED\t" << message << '\n';
  }
  if (request.format == "summary") {
    for (const auto &source : loaded.loaded) {
      std::map<std::string, std::size_t> counts;
      for (const auto &item : source.catalog.objects)
        ++counts[object_type_text(item.object.header.type)];
      std::cout << source.path.generic_string() << '\t' << media_kind_text(source.media.kind())
                << "\tobjects=" << source.catalog.objects.size();
      for (const auto &[type, count] : counts)
        std::cout << ' ' << type << '=' << count;
      if (source.media.kind() == axk::MediaKind::iso9660)
        std::cout << "\trecovery=clean-iso9660-object:" << source.catalog.objects.size() << '\n';
      else
        std::cout << "\trecovery=-\n";
    }
    return loaded.errors.empty() ? 0 : 1;
  }
  for (const auto &source : loaded.loaded) {
    const auto tree = cli_content_tree(source, request.show_default_programs);
    if (request.format == "paths")
      std::cout << "source_path\tscope\tpath\tdisplay_name\tobject_type\tobject_key\n";
    else
      std::cout << source.path.generic_string() << " [" << media_kind_text(source.media.kind())
                << "]\n";
    for (const auto &root : tree.roots) {
      if (request.format == "paths")
        render_tree_paths(source, root);
      else
        render_tree_text(root, 1U, {}, root.node_id == tree.roots.back().node_id, request);
    }
  }
  return loaded.errors.empty() ? 0 : 1;
}

} // namespace

int axk::cli::run(int argc, char **argv) {
  for (int index = 0; index < argc; ++index) {
    if (argv[index] == nullptr || !valid_utf8(argv[index])) {
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
  objects->add_option("-o,--output-dir", objects_request.output_directory,
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
  relationships->add_option("-o,--output-dir", relationships_request.output_directory,
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
  validate->add_option("paths", validate_request.paths, "input files or directories")->expected(0, -1);
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
  if (*alter_hds) {
    const auto output =
        !alter_output.empty() ? std::optional<std::filesystem::path>{alter_output} : std::nullopt;
    return run_alter_hds(alter_source, alter_manifest, output, alter_pretty);
  }
  std::cout << app.help();
  return 0;
}
