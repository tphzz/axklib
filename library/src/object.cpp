#include "axklib/object.hpp"

#include <algorithm>
#include <array>
#include <string_view>

#include "axklib/bytes.hpp"
#include "axklib/generated/current_sbnk_fields.hpp"
#include "axklib/lookups.hpp"
#include "axklib/sequence.hpp"

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
FieldValue<T> field(T value, std::uint32_t offset, std::uint32_t size, Verification verification, std::string basis) {
    return {std::move(value), {offset, size, verification, std::move(basis)}};
}

template <std::size_t Size>
FieldValue<std::array<std::byte, Size>> byte_field(std::span<const std::byte> payload, std::size_t offset,
                                                   Verification verification, std::string basis) {
    std::array<std::byte, Size> value{};
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(offset), Size, value.begin());
    return field(value, static_cast<std::uint32_t>(offset), static_cast<std::uint32_t>(Size), verification,
                 std::move(basis));
}

Result<CurrentObjectCommonRecord> decode_current_common_record(std::span<const std::byte> payload) {
    if (payload.size() < 0x7bU) {
        return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                          "current object common record requires at least 123 bytes")};
    }
    const ByteReader reader{payload};
    const auto object_class = reader.u8(0x30U);
    const auto state = reader.u8(0x31U);
    const auto name = reader.printable_ascii_field(0x32U, 16U);
    const auto state_0x42 = reader.u8(0x42U);
    const auto embedded_container_name = reader.printable_ascii_field(0x54U, 16U);
    const auto transient_name_hash_alias = reader.be32(0x68U);
    const auto transient_name_hash_next_handle = reader.be32(0x74U);
    if (!object_class || !state || !name || !state_0x42 || !embedded_container_name || !transient_name_hash_alias ||
        !transient_name_hash_next_handle) {
        return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                          "current object common record is truncated")};
    }

    CurrentObjectCommonRecord result{
        field(*object_class, 0x30U, 1U, Verification::verified, "verified current-object class dispatch"),
        field(*state, 0x31U, 1U, Verification::unknown, "opaque-preserved current-object state"),
        field(*name, 0x32U, 16U, Verification::verified, "verified current-object Common name"),
        field(*state_0x42, 0x42U, 1U, Verification::unknown,
              "opaque-preserved state transferred by the Common-record load/save transforms"),
        byte_field<7>(payload, 0x43U, Verification::verified, "serializer leaves this range unwritten"),
        byte_field<10>(payload, 0x4aU, Verification::unknown, "opaque-preserved current-object Common state"),
        field(*embedded_container_name, 0x54U, 16U, Verification::corroborated,
              "current-object Common reserved text and corpus source/container correlation"),
        byte_field<4>(payload, 0x64U, Verification::unknown, "opaque-preserved current-object Common state"),
        field(*transient_name_hash_alias, 0x68U, 4U, Verification::verified, "Common-record serializer alias"),
        byte_field<3>(payload, 0x6cU, Verification::verified, "body-prefix serializer alias"),
        byte_field<5>(payload, 0x6fU, Verification::verified, "serializer leaves this range unwritten"),
        field(*transient_name_hash_next_handle, 0x74U, 4U, Verification::verified, "runtime name-hash collision chain"),
    };
    result.transient_name_hash_alias_matches = *transient_name_hash_alias == *transient_name_hash_next_handle;
    result.body_prefix_alias_matches = std::ranges::equal(result.body_prefix_alias.value, payload.subspan(0x78U, 3U));
    return result;
}

Result<CurrentSmpl> decode_smpl(std::span<const std::byte> payload, const ObjectHeader &header) {
    if (payload.size() < 0xacU) {
        return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                          "current SMPL compact record requires at least 172 bytes")};
    }
    const ByteReader reader{payload};
    const auto sample_rate = reader.be16(0x28);
    const auto sample_width = reader.be16(0x2a);
    const auto embedded_container_name = reader.printable_ascii_field(0x54, 16);
    const auto transient_name_hash_next_handle = reader.be32(0x74);
    const auto reference_value = reader.be32(0x78);
    const auto duplicate_rate = reader.be16(0x7c);
    const auto root_key = reader.u8(0x7e);
    const auto fine_tune = reader.s8(0x7f);
    const auto pcm_transfer_control = reader.u8(0x84);
    const auto loop_mode = reader.u8(0x85);
    const auto wave_start = reader.be32(0x8e);
    const auto wave_length = reader.be32(0x92);
    const auto loop_start = reader.be32(0x96);
    const auto loop_length = reader.be32(0x9a);
    const auto transient_512_byte_block_counter = reader.be16(0xaa);
    if (!sample_rate || !sample_width || !embedded_container_name || !transient_name_hash_next_handle ||
        !reference_value || !duplicate_rate || !root_key || !fine_tune || !pcm_transfer_control || !loop_mode ||
        !wave_start || !wave_length || !loop_start || !loop_length || !transient_512_byte_block_counter) {
        return std::unexpected{
            make_error(ErrorCode::container_truncated, ErrorCategory::object, "current SMPL metadata is truncated")};
    }
    CurrentSmpl result{
        field(*sample_rate, 0x28, 2, Verification::corroborated, "current SMPL header"),
        field(*sample_width, 0x2a, 2, Verification::corroborated, "current SMPL header"),
        field(*embedded_container_name, 0x54, 16, Verification::verified,
              "A-series path descriptor and controlled SFS Volume rename"),
        field(*transient_name_hash_next_handle, 0x74, 4, Verification::verified,
              "A-series runtime name-hash collision chain"),
        field(*reference_value, 0x78, 4, Verification::corroborated, "compact Wave Data reference value"),
        field(*duplicate_rate, 0x7c, 2, Verification::corroborated, "compact rate copy"),
        field(*root_key, 0x7e, 1, Verification::corroborated, "compact pitch field"),
        field(*fine_tune, 0x7f, 1, Verification::corroborated, "compact pitch field"),
        field(*pcm_transfer_control, 0x84, 1, Verification::verified, "A-series PCM transfer-format selection"),
        static_cast<std::uint8_t>(*pcm_transfer_control & 0x30U),
        field(*loop_mode, 0x85, 1, Verification::corroborated, "compact loop field"),
        {},
        field(*wave_start, 0x8e, 4, Verification::verified, "Wave Data playback window"),
        field(*wave_length, 0x92, 4, Verification::corroborated, "compact frame field"),
        std::nullopt,
        field(*loop_start, 0x96, 4, Verification::corroborated, "compact loop field"),
        field(*loop_length, 0x9a, 4, Verification::corroborated, "compact loop field"),
        std::nullopt,
        std::nullopt,
        field(*transient_512_byte_block_counter, 0xaa, 2, Verification::verified,
              "A-series 512-byte transfer counter and controlled hardware validation"),
        header.header_size,
        header.payload_bytes_0x1c,
        header.payload_offset_0x24,
        header.payload_bytes_0x20,
        {},
    };
    result.loop_mode_label = current_label(CurrentLookup::current_smpl_loop_mode_labels, *loop_mode);
    const auto wave_end = checked_add(*wave_start, *wave_length);
    if (!wave_end)
        return std::unexpected{wave_end.error()};
    result.wave_end_frame_exclusive = *wave_end;
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
    const auto cached_reference = reader.be32(right ? 0xa4U : 0xa0U);
    const auto root = reader.u8(right ? 0xd7U : 0xd6U);
    const auto rate = reader.be16(right ? 0xdaU : 0xd8U);
    const auto fine = reader.s8(right ? 0xddU : 0xdcU);
    const auto pitch = reader.be16(right ? 0xe0U : 0xdeU);
    const auto start = reader.be32(right ? 0xecU : 0xe8U);
    const auto length = reader.be32(right ? 0xf4U : 0xf0U);
    const auto loop_start = reader.be32(right ? 0xfcU : 0xf8U);
    const auto loop_length = reader.be32(right ? 0x104U : 0x100U);
    if (!name || !cached_reference || !root || !rate || !fine || !pitch || !start || !length || !loop_start ||
        !loop_length) {
        return std::unexpected{
            make_error(ErrorCode::container_truncated, ErrorCategory::object, "current SBNK member lane is truncated")};
    }
    return CurrentSbnkMember{.wave_data_name = *name,
                             .cached_wave_data_reference_value = *cached_reference,
                             .root_key = *root,
                             .sample_rate = *rate,
                             .fine_tune_cents = *fine,
                             .pitch_base_word = *pitch,
                             .wave_start_frame = *start,
                             .wave_length_frames = *length,
                             .loop_start_frame = *loop_start,
                             .loop_length_frames = *loop_length};
}

Result<CurrentSbnk> decode_sbnk(std::span<const std::byte> payload, const ObjectHeader &header) {
    if (payload.size() < 0x108U) {
        return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                          "current SBNK member contract requires at least 264 bytes")};
    }
    const ByteReader reader{payload};
    const auto common = decode_current_common_record(payload);
    const auto sample_name = reader.printable_ascii_field(0x32, 16);
    const auto left = decode_sbnk_member(reader, false);
    const auto inactive_right = decode_sbnk_member(reader, true);
    if (!common || !sample_name || !left || !inactive_right) {
        if (!common)
            return std::unexpected{common.error()};
        return std::unexpected{!left ? left.error()
                                     : (!inactive_right ? inactive_right.error()
                                                        : make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                                                     "current SBNK names are malformed"))};
    }
    CurrentSbnk result;
    result.common = *common;
    result.sample_name = *sample_name;
    result.left = *left;
    result.inactive_right = *inactive_right;
    result.right_slot_present = !inactive_right->wave_data_name.empty();
    if (result.right_slot_present) {
        result.right = *inactive_right;
        result.right_link_role = "sample-reference";
    } else if (inactive_right->cached_wave_data_reference_value == 0) {
        result.right_link_role = "unused-zero";
    } else if (inactive_right->cached_wave_data_reference_value == left->cached_wave_data_reference_value) {
        result.right_link_role = "unused-mirrors-left-cache";
    } else {
        result.right_link_role = "unused-nonzero";
    }
    for (std::size_t word_index = 0; word_index < result.linked_program_bitmap_words.size(); ++word_index) {
        const auto word = reader.be32(0xc0U + word_index * 4U);
        if (!word) {
            return std::unexpected{word.error()};
        }
        result.linked_program_bitmap_words[word_index] = *word;
        for (std::uint8_t bit = 0; bit < 32U; ++bit) {
            if ((*word & (std::uint32_t{1} << bit)) != 0) {
                result.linked_program_numbers.push_back(static_cast<std::uint8_t>(word_index * 32U + bit + 1U));
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
    const auto loop_mode = reader.u8(0xe5);
    if (!sample_flags || !mapout_flags || !key_high || !key_low || !level || !pan || !velocity_high || !velocity_low ||
        !loop_mode) {
        return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                          "current SBNK parameter window is truncated")};
    }
    result.sample_flags = *sample_flags;
    result.mapout_flags = *mapout_flags;
    result.uses_program_portamento = (*mapout_flags & 0x01U) != 0U;
    result.mono_mode = (*mapout_flags & 0x02U) != 0U;
    result.legacy_velocity_xfade_default_5 = (*mapout_flags & 0x08U) != 0U;
    result.key_range_high = *key_high;
    result.key_range_low = *key_low;
    result.sample_level = *level;
    result.pan = *pan;
    result.velocity_range_high = *velocity_high;
    result.velocity_range_low = *velocity_low;
    result.loop_mode = *loop_mode;
    result.loop_mode_label = current_label(CurrentLookup::current_smpl_loop_mode_labels, *loop_mode);
    constexpr std::size_t control_count = 6U;
    constexpr std::size_t control_size = 4U;
    constexpr std::size_t control_bytes = control_count * control_size;
    constexpr std::size_t compatibility_control_offset = 0x0a8U;
    constexpr std::size_t tail_control_offset = 0x164U;
    constexpr std::size_t object_prefix_size = 0x30U;
    const auto declared_size = object_prefix_size + static_cast<std::size_t>(header.payload_bytes_0x1c);
    const auto logical_size =
        header.payload_bytes_0x1c == 0U ? payload.size() : std::min(payload.size(), declared_size);
    result.control_record_tail_copy_present = logical_size >= tail_control_offset + control_bytes;
    result.control_record_storage_offset =
        result.control_record_tail_copy_present ? tail_control_offset : compatibility_control_offset;
    if (result.control_record_tail_copy_present) {
        result.control_record_copies_match =
            std::ranges::equal(payload.subspan(compatibility_control_offset, control_bytes),
                               payload.subspan(tail_control_offset, control_bytes));
    }
    for (std::size_t index = 0; index < control_count; ++index) {
        const auto offset = result.control_record_storage_offset + index * control_size;
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
        Result<std::int64_t> value = std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
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
            {descriptor.offset, descriptor.width, Verification::corroborated, "current SBNK parameter field"},
        });
    }
    const auto parameter_end = std::max<std::size_t>(0x0a8U, std::min<std::size_t>(logical_size, 0x188U));
    result.raw_parameter_window.assign(payload.begin() + 0xa8,
                                       payload.begin() + static_cast<std::ptrdiff_t>(parameter_end));
    return result;
}

Result<CurrentSbac> decode_sbac(std::span<const std::byte> payload, const ObjectHeader &header) {
    constexpr std::size_t parameter_prefix_offset = 0x78U;
    constexpr std::size_t parameter_prefix_size = 0xbcU;
    constexpr std::size_t parameter_tail_size = 0x24U;
    constexpr std::size_t pending_parameter_bitmap_offset = 0x134U;
    constexpr std::size_t member_count_offset = 0x144U;
    constexpr std::size_t first_member_offset = 0x14cU;
    constexpr std::size_t member_size = 0x14U;
    if (payload.size() <= 0x144U) {
        return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                          "current SBAC payload is too short for its member count")};
    }
    const ByteReader reader{payload};
    CurrentSbac result;
    const auto common = decode_current_common_record(payload);
    if (!common)
        return std::unexpected{common.error()};
    result.common = *common;
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(parameter_prefix_offset), parameter_prefix_size,
                result.raw_sample_parameter_block.begin());
    auto member_region_end = payload.size();
    if (header.unknown_0x14 >= 4U) {
        if (payload.size() < first_member_offset + parameter_tail_size) {
            return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                              "current SBAC payload is too short for its split parameter tail")};
        }
        result.storage_layout = SbacStorageLayout::current_split_parameter_tail;
        result.parameter_tail_offset = payload.size() - parameter_tail_size;
        member_region_end = *result.parameter_tail_offset;
        std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(*result.parameter_tail_offset), parameter_tail_size,
                    result.raw_sample_parameter_block.begin() + static_cast<std::ptrdiff_t>(parameter_prefix_size));
    }
    for (std::size_t word_index = 0; word_index < result.pending_parameter_propagation_words.size(); ++word_index) {
        const auto word = reader.be32(pending_parameter_bitmap_offset + word_index * 4U);
        if (!word) {
            return std::unexpected{word.error()};
        }
        result.pending_parameter_propagation_words[word_index] = *word;
        for (std::uint8_t bit = 0; bit < 32U; ++bit) {
            if ((*word & (std::uint32_t{1} << bit)) == 0) {
                continue;
            }
            const auto number = static_cast<std::uint8_t>(word_index * 32U + bit);
            (number <= 88U ? result.pending_parameter_numbers : result.reserved_pending_parameter_numbers)
                .push_back(number);
        }
    }
    const auto member_count = reader.u8(member_count_offset);
    if (!member_count) {
        return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                          "current SBAC member count is truncated")};
    }
    result.stored_member_count = *member_count;
    result.maximum_member_count =
        member_region_end < first_member_offset ? 0U : (member_region_end - first_member_offset) / member_size;
    const auto decoded_slots = std::min<std::size_t>(*member_count, result.maximum_member_count);
    for (std::size_t index = 0; index < decoded_slots; ++index) {
        const auto offset = first_member_offset + index * member_size;
        const auto name = reader.printable_ascii_field(offset, 16);
        const auto handle = reader.be32(offset + 16U);
        if (!name || !handle) {
            return std::unexpected{
                make_error(ErrorCode::container_truncated, ErrorCategory::object, "current SBAC slot is truncated")};
        }
        const auto active = payload[offset] != std::byte{};
        result.slots.push_back({*name, active, *handle, static_cast<std::uint32_t>(offset)});
        if (active)
            ++result.effective_member_count;
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
        if (!name || !handle || !kind || !flags || !level || !velocity || !pan || !key_high || !key_low ||
            !velocity_high || !velocity_low) {
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

Result<CurrentRecordEnvelope> decode_current_record_envelope(std::span<const std::byte> payload) {
    if (payload.size() < current_record_envelope_size) {
        return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                          "current record envelope requires at least 48 bytes")};
    }
    if (!begins_with(payload, object_magic)) {
        return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                          "record does not begin with current record magic")};
    }
    const auto raw_type = ByteReader{payload}.ascii_field(0x0cU, 4U, false);
    if (!raw_type)
        return std::unexpected{raw_type.error()};
    CurrentRecordEnvelope result;
    result.type = object_type(*raw_type);
    result.raw_type = *raw_type;
    std::copy_n(payload.begin(), result.raw_bytes.size(), result.raw_bytes.begin());
    return result;
}

Result<ObjectHeader> decode_object_header(std::span<const std::byte> payload) {
    if (payload.size() < 0x42U) {
        return std::unexpected{make_error(ErrorCode::container_truncated, ErrorCategory::object,
                                          "object header requires at least 66 bytes")};
    }
    const auto envelope = decode_current_record_envelope(payload);
    if (!envelope)
        return std::unexpected{envelope.error()};
    const ByteReader reader{payload};
    const auto name = reader.printable_ascii_field(0x32, 16);
    const auto header_size = reader.be32(0x10);
    const auto unknown_14 = reader.be32(0x14);
    const auto record_size = reader.be32(0x18);
    const auto payload_1c = reader.be32(0x1c);
    const auto payload_20 = reader.be32(0x20);
    const auto payload_offset = reader.be32(0x24);
    if (!name || !header_size || !unknown_14 || !record_size || !payload_1c || !payload_20 || !payload_offset) {
        return std::unexpected{
            make_error(ErrorCode::container_truncated, ErrorCategory::object, "object header fields are truncated")};
    }
    ObjectHeader result;
    result.type = envelope->type;
    result.raw_type = envelope->raw_type;
    result.name = *name;
    result.header_size = *header_size;
    result.unknown_0x14 = *unknown_14;
    result.record_size_or_header_used = *record_size;
    result.payload_bytes_0x1c = *payload_1c;
    result.payload_bytes_0x20 = *payload_20;
    result.payload_offset_0x24 = *payload_offset;
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
        const auto decoded = decode_sbnk(payload, *header);
        if (!decoded) {
            return std::unexpected{decoded.error()};
        }
        return DecodedObject{*header, ObjectFormat::current, *decoded};
    }
    if (header->type == ObjectType::sbac) {
        const auto decoded = decode_sbac(payload, *header);
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
        const auto decoded = decode_current_sequence(payload);
        if (!decoded)
            return std::unexpected{decoded.error()};
        return DecodedObject{*header, ObjectFormat::current, *decoded};
    }
    if (header->type == ObjectType::prf3) {
        return DecodedObject{*header, ObjectFormat::current, CurrentProfile{{payload.begin(), payload.end()}}};
    }
    return DecodedObject{
        *header,
        ObjectFormat::current,
        GenericObject{std::vector<std::byte>{payload.begin(), payload.end()}},
    };
}

} // namespace axk
