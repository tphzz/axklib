#include "objects_v1.hpp"

#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>

namespace axk::cli::schema::objects_v1 {
namespace {

using OrderedJson = nlohmann::ordered_json;

std::string hex(std::span<const std::byte> bytes) {
  std::ostringstream result;
  result << std::hex << std::setfill('0');
  for (const auto byte : bytes)
    result << std::setw(2) << static_cast<unsigned int>(std::to_integer<std::uint8_t>(byte));
  return result.str();
}

OrderedJson member_json(const CurrentSbnkMember &member) {
  return {{"sample_name", member.sample_name},
          {"smpl_link_id", member.smpl_link_id},
          {"root_key", member.root_key},
          {"sample_rate", member.sample_rate},
          {"fine_tune_cents", member.fine_tune_cents},
          {"pitch_base_word", member.pitch_base_word},
          {"wave_length_frames", member.wave_length_frames},
          {"loop_start_frame", member.loop_start_frame},
          {"loop_length_frames", member.loop_length_frames}};
}

OrderedJson decoded_json(const DecodedObject &object) {
  if (const auto *sample = std::get_if<CurrentSmpl>(&object.payload)) {
    return {{"kind", "SMPL"},
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
            {"compact_record_hex", hex(sample->compact_record)}};
  }
  if (const auto *bank = std::get_if<CurrentSbnk>(&object.payload)) {
    auto fields = OrderedJson::object();
    for (const auto &field : bank->numeric_fields)
      fields[field.name] = field.value ? OrderedJson(*field.value) : OrderedJson(nullptr);
    auto controls = OrderedJson::array();
    for (const auto &control : bank->control_records)
      controls.push_back({control.device, control.function, control.type, control.range});
    return {{"kind", "SBNK"},
            {"bank_name", bank->bank_name},
            {"instrument_name", bank->instrument_name},
            {"right_slot_present", bank->right_slot_present},
            {"right_link_role", bank->right_link_role},
            {"left", member_json(bank->left)},
            {"right", bank->right ? member_json(*bank->right) : OrderedJson(nullptr)},
            {"inactive_right", member_json(bank->inactive_right)},
            {"linked_program_bitmap_words", bank->linked_program_bitmap_words},
            {"linked_program_numbers", bank->linked_program_numbers},
            {"numeric_fields", std::move(fields)},
            {"control_records", std::move(controls)},
            {"raw_parameter_window_hex", hex(bank->raw_parameter_window)}};
  }
  if (const auto *group = std::get_if<CurrentSbac>(&object.payload)) {
    auto slots = OrderedJson::array();
    for (const auto &slot : group->slots)
      slots.push_back(
          {{"name", slot.name}, {"raw_handle", slot.raw_handle}, {"offset", slot.offset}});
    return {{"kind", "SBAC"},
            {"raw_sample_parameter_block_hex", hex(group->raw_sample_parameter_block)},
            {"value_enable_words", group->value_enable_words},
            {"enabled_parameter_numbers", group->enabled_parameter_numbers},
            {"enabled_numbers_outside_table", group->enabled_numbers_outside_table},
            {"bulk_assigned_sample_count", group->bulk_assigned_sample_count},
            {"active_slot_count", group->active_slot_count},
            {"maximum_slot_count", group->maximum_slot_count},
            {"slots", std::move(slots)}};
  }
  if (const auto *program = std::get_if<CurrentProg>(&object.payload)) {
    auto assignments = OrderedJson::array();
    for (const auto &row : program->assignments) {
      assignments.push_back({{"name", row.name},
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
                             {"raw_row_hex", hex(row.raw_row)}});
    }
    auto effects = OrderedJson::array();
    for (const auto &block : program->effect_blocks)
      effects.push_back(hex(block));
    return {{"kind", "PROG"},
            {"raw_control_block_hex", hex(program->raw_control_block)},
            {"raw_control_tail_copy_hex", hex(program->raw_control_tail_copy)},
            {"effect_blocks_hex", std::move(effects)},
            {"assignments", std::move(assignments)}};
  }
  if (const auto *sequence = std::get_if<CurrentSequence>(&object.payload))
    return {{"kind", "SEQU"}, {"raw_payload_hex", hex(sequence->raw_payload)}};
  if (const auto *profile = std::get_if<CurrentProfile>(&object.payload))
    return {{"kind", "PRF3"}, {"raw_payload_hex", hex(profile->raw_payload)}};
  return {{"kind", "generic"}};
}

OrderedJson header_json(const ObjectHeader &header) {
  return {{"raw_type", header.raw_type},
          {"name", header.name},
          {"header_size", header.header_size},
          {"unknown_0x14", header.unknown_0x14},
          {"record_size_or_header_used", header.record_size_or_header_used},
          {"payload_bytes_0x1c", header.payload_bytes_0x1c},
          {"payload_bytes_0x20", header.payload_bytes_0x20},
          {"raw_prefix_hex", hex(header.raw_prefix)}};
}

} // namespace

Result<std::string> serialize(const ObjectsOutput &output, bool pretty) {
  try {
    auto objects = OrderedJson::array();
    for (const auto &object : output.objects) {
      if (output.shape == ContainerShape::sfs) {
        objects.push_back({{"partition_index", *object.partition_index},
                           {"sfs_id", *object.sfs_id},
                           {"header", header_json(object.header)},
                           {"decoded", decoded_json(object.decoded)}});
      } else {
        objects.push_back({{"key", object.key},
                           {"logical_path", object.logical_path},
                           {"scope_key", object.scope_key},
                           {"raw_group", object.raw_group},
                           {"raw_volume", object.raw_volume},
                           {"group_label", object.group_label},
                           {"group_label_status", object.group_label_status},
                           {"group_label_basis", object.group_label_basis},
                           {"volume_label", object.volume_label},
                           {"volume_label_status", object.volume_label_status},
                           {"volume_label_basis", object.volume_label_basis},
                           {"data_offset", object.data_offset},
                           {"size", object.size},
                           {"structured_path", object.structured_path_utf8},
                           {"header", header_json(object.header)},
                           {"decoded", decoded_json(object.decoded)}});
      }
    }
    auto root = OrderedJson{{"schema_version", schema_version}};
    if (output.shape == ContainerShape::media)
      root["container"] = output.container_kind;
    root["objects"] = std::move(objects);
    return root.dump(pretty ? 2 : -1);
  } catch (const nlohmann::json::exception &error) {
    return std::unexpected{
        make_error(ErrorCode::invalid_argument, ErrorCategory::internal,
                   std::string{"could not serialize object JSON: "} + error.what())};
  }
}

} // namespace axk::cli::schema::objects_v1
