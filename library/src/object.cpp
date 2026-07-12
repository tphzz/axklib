#include "axklib/object.hpp"

#include <algorithm>
#include <array>
#include <string_view>

#include "axklib/bytes.hpp"
#include "axklib/generated/current_sbnk_fields.hpp"
#include "axklib/lookups.hpp"

namespace axk {
namespace {

constexpr std::string_view object_magic{"FSFSDEV3SPLX"};

bool begins_with(std::span<const std::byte> bytes, std::string_view value) {
  return bytes.size() >= value.size() &&
         std::equal(value.begin(), value.end(), bytes.begin(), [](char left, std::byte right) {
           return static_cast<unsigned char>(left) == std::to_integer<unsigned char>(right);
         });
}

ObjectType object_type(std::string_view raw) {
  if (raw == "SMPL")
    return ObjectType::smpl;
  if (raw == "SBNK")
    return ObjectType::sbnk;
  if (raw == "SBAC")
    return ObjectType::sbac;
  if (raw == "PROG")
    return ObjectType::prog;
  if (raw == "SEQU")
    return ObjectType::sequ;
  if (raw == "PRF3")
    return ObjectType::prf3;
  return ObjectType::unknown;
}

template <typename T>
FieldValue<T> field(T value, std::uint32_t offset, std::uint32_t size, Verification verification,
                    std::string basis) {
  return {std::move(value), {offset, size, verification, std::move(basis)}};
}

Result<CurrentSmpl> decode_smpl(std::span<const std::byte> payload, const ObjectHeader &header) {
  if (payload.size() < 0xacU) {
    return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                      "current SMPL compact record requires at least 172 bytes")};
  }
  const ByteReader reader{payload};
  const auto sample_rate = reader.be16(0x28);
  const auto sample_width = reader.be16(0x2a);
  const auto source_name = reader.printable_ascii_field(0x54, 16);
  const auto group_id = reader.be32(0x6c);
  const auto link_id = reader.be32(0x78);
  const auto duplicate_rate = reader.be16(0x7c);
  const auto root_key = reader.u8(0x7e);
  const auto fine_tune = reader.s8(0x7f);
  const auto loop_mode = reader.u8(0x85);
  const auto wave_length = reader.be32(0x92);
  const auto loop_start = reader.be32(0x96);
  const auto loop_length = reader.be32(0x9a);
  if (!sample_rate || !sample_width || !source_name || !group_id || !link_id || !duplicate_rate ||
      !root_key || !fine_tune || !loop_mode || !wave_length || !loop_start || !loop_length) {
    return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                      "current SMPL metadata is truncated")};
  }
  CurrentSmpl result{
      field(*sample_rate, 0x28, 2, Verification::corroborated, "current SMPL header"),
      field(*sample_width, 0x2a, 2, Verification::corroborated, "current SMPL header"),
      field(*source_name, 0x54, 16, Verification::corroborated, "compact record text"),
      field(*group_id, 0x6c, 4, Verification::corroborated, "compact record link field"),
      field(*link_id, 0x78, 4, Verification::corroborated, "compact record link field"),
      field(*duplicate_rate, 0x7c, 2, Verification::corroborated, "compact rate copy"),
      field(*root_key, 0x7e, 1, Verification::corroborated, "compact pitch field"),
      field(*fine_tune, 0x7f, 1, Verification::corroborated, "compact pitch field"),
      field(*loop_mode, 0x85, 1, Verification::corroborated, "compact loop field"),
      {},
      field(*wave_length, 0x92, 4, Verification::corroborated, "compact frame field"),
      field(*loop_start, 0x96, 4, Verification::corroborated, "compact loop field"),
      field(*loop_length, 0x9a, 4, Verification::corroborated, "compact loop field"),
      std::nullopt,
      std::nullopt,
      header.header_size,
      header.payload_bytes_0x1c,
      {},
  };
  result.loop_mode_label = current_label(CurrentLookup::current_smpl_loop_mode_labels, *loop_mode);
  if (*loop_length != 0) {
    const auto exclusive = checked_add(*loop_start, *loop_length);
    if (!exclusive)
      return std::unexpected{exclusive.error()};
    result.loop_end_frame_exclusive = *exclusive;
    result.loop_end_frame_inclusive = *exclusive - 1U;
  }
  std::copy_n(payload.begin() + 0x30, result.compact_record.size(), result.compact_record.begin());
  return result;
}

Result<CurrentSbnkMember> decode_sbnk_member(const ByteReader &reader, bool right) {
  const auto name = reader.printable_ascii_field(right ? 0x88U : 0x78U, 16);
  const auto link = reader.be32(right ? 0xa4U : 0xa0U);
  const auto root = reader.u8(right ? 0xd7U : 0xd6U);
  const auto rate = reader.be16(right ? 0xdaU : 0xd8U);
  const auto fine = reader.s8(right ? 0xddU : 0xdcU);
  const auto pitch = reader.be16(right ? 0xe0U : 0xdeU);
  const auto length = reader.be32(right ? 0xf4U : 0xf0U);
  const auto loop_start = reader.be32(right ? 0xfcU : 0xf8U);
  const auto loop_length = reader.be32(right ? 0x104U : 0x100U);
  if (!name || !link || !root || !rate || !fine || !pitch || !length || !loop_start ||
      !loop_length) {
    return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                      "current SBNK member lane is truncated")};
  }
  return CurrentSbnkMember{*name,  *link,   *root,       *rate,       *fine,
                           *pitch, *length, *loop_start, *loop_length};
}

Result<CurrentSbnk> decode_sbnk(std::span<const std::byte> payload) {
  if (payload.size() < 0x108U) {
    return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                      "current SBNK member contract requires at least 264 bytes")};
  }
  const ByteReader reader{payload};
  const auto bank_name = reader.printable_ascii_field(0x32, 16);
  const auto instrument_name = reader.printable_ascii_field(0x50, 24);
  const auto left = decode_sbnk_member(reader, false);
  const auto inactive_right = decode_sbnk_member(reader, true);
  if (!bank_name || !instrument_name || !left || !inactive_right) {
    return std::unexpected{
        !left ? left.error()
              : (!inactive_right ? inactive_right.error()
                                 : make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                              "current SBNK names are malformed"))};
  }
  CurrentSbnk result;
  result.bank_name = *bank_name;
  result.instrument_name = *instrument_name;
  result.left = *left;
  result.inactive_right = *inactive_right;
  result.right_slot_present = !inactive_right->sample_name.empty();
  if (result.right_slot_present) {
    result.right = *inactive_right;
    result.right_link_role = "sample-reference";
  } else if (inactive_right->smpl_link_id == 0) {
    result.right_link_role = "unused-zero";
  } else if (inactive_right->smpl_link_id == left->smpl_link_id) {
    result.right_link_role = "unused-mirrors-left-link";
  } else {
    result.right_link_role = "unused-nonzero";
  }
  for (std::size_t word_index = 0; word_index < result.linked_program_bitmap_words.size();
       ++word_index) {
    const auto word = reader.be32(0xc0U + word_index * 4U);
    if (!word) {
      return std::unexpected{word.error()};
    }
    result.linked_program_bitmap_words[word_index] = *word;
    for (std::uint8_t bit = 0; bit < 32U; ++bit) {
      if ((*word & (std::uint32_t{1} << bit)) != 0) {
        result.linked_program_numbers.push_back(
            static_cast<std::uint8_t>(word_index * 32U + bit + 1U));
      }
    }
  }
  const auto sample_flags = reader.u8(0xd0);
  const auto mapout_flags = reader.u8(0xd1);
  const auto key_high = reader.u8(0xe2);
  const auto key_low = reader.u8(0xe3);
  const auto level = reader.u8(0x116);
  const auto pan = reader.s8(0x117);
  const auto velocity_high = reader.u8(0x11a);
  const auto velocity_low = reader.u8(0x11b);
  if (!sample_flags || !mapout_flags || !key_high || !key_low || !level || !pan || !velocity_high ||
      !velocity_low) {
    return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                      "current SBNK parameter window is truncated")};
  }
  result.sample_flags = *sample_flags;
  result.mapout_flags = *mapout_flags;
  result.key_range_high = *key_high;
  result.key_range_low = *key_low;
  result.sample_level = *level;
  result.pan = *pan;
  result.velocity_range_high = *velocity_high;
  result.velocity_range_low = *velocity_low;
  constexpr std::size_t control_count = 6;
  for (std::size_t index = 0; index < control_count; ++index) {
    const auto offset = 0x164U + index * 4U;
    if (offset + 4U > payload.size())
      break;
    const auto device = reader.u8(offset);
    const auto function = reader.u8(offset + 1U);
    const auto type = reader.u8(offset + 2U);
    const auto range = reader.s8(offset + 3U);
    if (!device || !function || !type || !range) {
      return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                        "current SBNK control record is truncated")};
    }
    result.control_records.push_back({*device, *function, *type, *range});
  }
  result.numeric_fields.reserve(generated::sbnk_numeric_fields.size());
  for (const auto &descriptor : generated::sbnk_numeric_fields) {
    Result<std::int64_t> value =
        std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                   "unsupported generated numeric field width")};
    if (descriptor.width == 1U) {
      value = descriptor.is_signed ? reader.s8(descriptor.offset).transform([](std::int8_t item) {
        return static_cast<std::int64_t>(item);
      })
                                   : reader.u8(descriptor.offset).transform([](std::uint8_t item) {
                                       return static_cast<std::int64_t>(item);
                                     });
    } else if (descriptor.width == 2U) {
      value = reader.be16(descriptor.offset).transform([](std::uint16_t item) {
        return static_cast<std::int64_t>(item);
      });
    } else if (descriptor.width == 4U) {
      value = reader.be32(descriptor.offset).transform([](std::uint32_t item) {
        return static_cast<std::int64_t>(item);
      });
    }
    result.numeric_fields.push_back({
        std::string{descriptor.name},
        value ? std::optional<std::int64_t>{*value} : std::nullopt,
        {descriptor.offset, descriptor.width, Verification::corroborated,
         "current SBNK parameter field"},
    });
  }
  const auto parameter_end = std::min<std::size_t>(payload.size(), 0x185U);
  result.raw_parameter_window.assign(payload.begin() + 0xa8,
                                     payload.begin() + static_cast<std::ptrdiff_t>(parameter_end));
  return result;
}

Result<CurrentSbac> decode_sbac(std::span<const std::byte> payload) {
  if (payload.size() <= 0x144U) {
    return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                      "current SBAC payload is too short for its slot count")};
  }
  const ByteReader reader{payload};
  CurrentSbac result;
  std::copy_n(payload.begin() + 0x40, result.raw_sample_parameter_block.size(),
              result.raw_sample_parameter_block.begin());
  for (std::size_t word_index = 0; word_index < result.value_enable_words.size(); ++word_index) {
    const auto word = reader.be32(0x120U + word_index * 4U);
    if (!word) {
      return std::unexpected{word.error()};
    }
    result.value_enable_words[word_index] = *word;
    for (std::uint8_t bit = 0; bit < 32U; ++bit) {
      if ((*word & (std::uint32_t{1} << bit)) == 0) {
        continue;
      }
      const auto number = static_cast<std::uint8_t>(word_index * 32U + bit);
      (number <= 88U ? result.enabled_parameter_numbers : result.enabled_numbers_outside_table)
          .push_back(number);
    }
  }
  const auto bulk_count = reader.u8(0x130);
  const auto slot_count = reader.u8(0x144);
  if (!bulk_count || !slot_count) {
    return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                      "current SBAC count fields are truncated")};
  }
  result.bulk_assigned_sample_count = *bulk_count;
  result.active_slot_count = *slot_count;
  result.maximum_slot_count = payload.size() < 0x14cU ? 0U : (payload.size() - 0x14cU) / 0x14U;
  const auto decoded_slots = std::min<std::size_t>(*slot_count, result.maximum_slot_count);
  for (std::size_t index = 0; index < decoded_slots; ++index) {
    const auto offset = 0x14cU + index * 0x14U;
    const auto name = reader.printable_ascii_field(offset, 16);
    const auto handle = reader.be32(offset + 16U);
    if (!name || !handle) {
      return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                        "current SBAC slot is truncated")};
    }
    result.slots.push_back({*name, *handle, static_cast<std::uint32_t>(offset)});
  }
  return result;
}

Result<CurrentProg> decode_prog(std::span<const std::byte> payload) {
  const ByteReader reader{payload};
  CurrentProg result;
  if (payload.size() >= 0x88U) {
    const auto program_name = reader.decoded_ascii_field(0x78, 8);
    if (!program_name)
      return std::unexpected{program_name.error()};
    result.program_name = *program_name;
  }
  constexpr std::size_t control_count = 4;
  for (std::size_t index = 0; index < control_count; ++index) {
    const auto offset = 0x110U + index * 4U;
    if (offset + 4U > payload.size())
      break;
    const auto device = reader.u8(offset);
    const auto function = reader.u8(offset + 1U);
    const auto type = reader.u8(offset + 2U);
    const auto range = reader.s8(offset + 3U);
    if (!device || !function || !type || !range) {
      return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                        "current PROG control record is truncated")};
    }
    result.control_records.push_back({*device, *function, *type, *range});
  }
  const auto slice = [&](std::size_t start, std::size_t end) {
    if (start >= payload.size())
      return std::vector<std::byte>{};
    end = std::min(end, payload.size());
    return std::vector<std::byte>{payload.begin() + static_cast<std::ptrdiff_t>(start),
                                  payload.begin() + static_cast<std::ptrdiff_t>(end)};
  };
  result.raw_control_block = slice(0x110, 0x120);
  result.raw_control_tail_copy = slice(0x358, 0x368);
  constexpr std::array effect_offsets{0x98U, 0xc0U, 0xe8U};
  for (std::size_t index = 0; index < effect_offsets.size(); ++index) {
    result.effect_blocks[index] = slice(effect_offsets[index], effect_offsets[index] + 0x28U);
  }
  const auto assignment_count = payload.size() < 0x120U ? 0U : (payload.size() - 0x120U) / 0x38U;
  for (std::size_t index = 0; index < assignment_count; ++index) {
    const auto offset = 0x120U + index * 0x38U;
    ProgAssignment assignment;
    const auto name = reader.decoded_ascii_field(offset, 16);
    const auto handle = reader.be32(offset + 0x10U);
    const auto kind = reader.u8(offset + 0x14U);
    const auto flags = reader.u8(offset + 0x15U);
    const auto level = reader.s8(offset + 0x16U);
    const auto velocity = reader.s8(offset + 0x17U);
    const auto pan = reader.s8(offset + 0x18U);
    const auto key_high = reader.u8(offset + 0x1eU);
    const auto key_low = reader.u8(offset + 0x1fU);
    const auto velocity_high = reader.u8(offset + 0x21U);
    const auto velocity_low = reader.u8(offset + 0x22U);
    if (!name || !handle || !kind || !flags || !level || !velocity || !pan || !key_high ||
        !key_low || !velocity_high || !velocity_low) {
      return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                        "current PROG assignment row is truncated")};
    }
    assignment.name = *name;
    assignment.raw_handle = *handle;
    assignment.kind = *kind;
    assignment.flags = *flags;
    assignment.level_offset = *level;
    assignment.velocity_sensitivity = *velocity;
    assignment.pan_offset = *pan;
    assignment.key_limit_high = *key_high;
    assignment.key_limit_low = *key_low;
    assignment.velocity_limit_high = *velocity_high;
    assignment.velocity_limit_low = *velocity_low;
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(offset), assignment.raw_row.size(),
                assignment.raw_row.begin());
    result.assignments.push_back(std::move(assignment));
  }
  return result;
}

} // namespace

const NumericField *CurrentSbnk::find_numeric_field(std::string_view name) const noexcept {
  const auto found = std::find_if(numeric_fields.begin(), numeric_fields.end(),
                                  [&](const NumericField &item) { return item.name == name; });
  return found == numeric_fields.end() ? nullptr : &*found;
}

Result<ObjectHeader> decode_object_header(std::span<const std::byte> payload) {
  if (payload.size() < 0x42U) {
    return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                      "object header requires at least 66 bytes")};
  }
  if (!begins_with(payload, object_magic)) {
    return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                      "object does not begin with current object magic")};
  }
  const ByteReader reader{payload};
  const auto raw_type = reader.ascii_field(0x0c, 4, false);
  const auto name = reader.printable_ascii_field(0x32, 16);
  const auto header_size = reader.be32(0x10);
  const auto unknown_14 = reader.be32(0x14);
  const auto record_size = reader.be32(0x18);
  const auto payload_1c = reader.be32(0x1c);
  const auto payload_20 = reader.be32(0x20);
  if (!raw_type || !name || !header_size || !unknown_14 || !record_size || !payload_1c ||
      !payload_20) {
    return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                      "object header fields are truncated")};
  }
  ObjectHeader result;
  result.type = object_type(*raw_type);
  result.raw_type = *raw_type;
  result.name = *name;
  result.header_size = *header_size;
  result.unknown_0x14 = *unknown_14;
  result.record_size_or_header_used = *record_size;
  result.payload_bytes_0x1c = *payload_1c;
  result.payload_bytes_0x20 = *payload_20;
  std::copy_n(payload.begin(), result.raw_prefix.size(), result.raw_prefix.begin());
  return result;
}

Result<DecodedObject> decode_object(std::span<const std::byte> payload) {
  const auto header = decode_object_header(payload);
  if (!header) {
    return std::unexpected{header.error()};
  }
  if (header->type == ObjectType::smpl) {
    const auto decoded = decode_smpl(payload, *header);
    if (!decoded) {
      return std::unexpected{decoded.error()};
    }
    return DecodedObject{*header, ObjectFormat::current, *decoded};
  }
  if (header->type == ObjectType::sbnk) {
    const auto decoded = decode_sbnk(payload);
    if (!decoded) {
      return std::unexpected{decoded.error()};
    }
    return DecodedObject{*header, ObjectFormat::current, *decoded};
  }
  if (header->type == ObjectType::sbac) {
    const auto decoded = decode_sbac(payload);
    if (!decoded)
      return std::unexpected{decoded.error()};
    return DecodedObject{*header, ObjectFormat::current, *decoded};
  }
  if (header->type == ObjectType::prog) {
    const auto decoded = decode_prog(payload);
    if (!decoded)
      return std::unexpected{decoded.error()};
    return DecodedObject{*header, ObjectFormat::current, *decoded};
  }
  if (header->type == ObjectType::sequ) {
    return DecodedObject{*header, ObjectFormat::current,
                         CurrentSequence{{payload.begin(), payload.end()}}};
  }
  if (header->type == ObjectType::prf3) {
    return DecodedObject{*header, ObjectFormat::current,
                         CurrentProfile{{payload.begin(), payload.end()}}};
  }
  return DecodedObject{
      *header,
      ObjectFormat::current,
      GenericObject{std::vector<std::byte>{payload.begin(), payload.end()}},
  };
}

} // namespace axk
