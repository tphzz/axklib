#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/export.hpp"

namespace axk {

enum class ObjectType : std::uint8_t {
    unknown,
    smpl,
    sbnk,
    sbac,
    prog,
    sequ,
    prf3,
};

enum class ObjectFormat : std::uint8_t { current, alternating_byte, unknown };
enum class Verification : std::uint8_t { verified, corroborated, tentative, unknown };

struct FieldSource {
    std::uint32_t offset{};
    std::uint32_t size{};
    Verification verification{Verification::unknown};
    std::string basis;
};

template <typename T> struct FieldValue {
    T value;
    FieldSource source;
};

struct ObjectHeader {
    ObjectType type{ObjectType::unknown};
    std::string raw_type;
    std::string name;
    std::uint32_t header_size{};
    std::uint32_t unknown_0x14{};
    std::uint32_t record_size_or_header_used{};
    std::uint32_t payload_bytes_0x1c{};
    std::uint32_t payload_bytes_0x20{};
    std::array<std::byte, 64> raw_prefix{};
};

struct CurrentSmpl {
    FieldValue<std::uint16_t> sample_rate;
    FieldValue<std::uint16_t> stored_sample_width_bytes;
    FieldValue<std::string> source_wave_name;
    FieldValue<std::uint32_t> group_id;
    FieldValue<std::uint32_t> link_id;
    FieldValue<std::uint16_t> duplicate_sample_rate;
    FieldValue<std::uint8_t> root_key;
    FieldValue<std::int8_t> fine_tune_cents;
    FieldValue<std::uint8_t> loop_mode;
    std::string loop_mode_label;
    FieldValue<std::uint32_t> wave_length_frames;
    FieldValue<std::uint32_t> loop_start_frame;
    FieldValue<std::uint32_t> loop_length_frames;
    std::optional<std::uint64_t> loop_end_frame_inclusive;
    std::optional<std::uint64_t> loop_end_frame_exclusive;
    std::uint32_t stored_pcm_offset{};
    std::uint32_t stored_pcm_bytes{};
    std::array<std::byte, 0x7c> compact_record{};
};

struct GenericObject {
    std::vector<std::byte> raw_payload;
};

struct CurrentSbnkMember {
    std::string sample_name;
    std::uint32_t smpl_link_id{};
    std::uint8_t root_key{};
    std::uint16_t sample_rate{};
    std::int8_t fine_tune_cents{};
    std::uint16_t pitch_base_word{};
    std::uint32_t wave_length_frames{};
    std::uint32_t loop_start_frame{};
    std::uint32_t loop_length_frames{};
};

struct SbnkControlRecord {
    std::uint8_t device{};
    std::uint8_t function{};
    std::uint8_t type{};
    std::int8_t range{};
};

struct NumericField {
    std::string name;
    std::optional<std::int64_t> value;
    FieldSource source;
};

struct CurrentSbnk {
    std::string bank_name;
    std::string instrument_name;
    bool right_slot_present{};
    std::string right_link_role;
    CurrentSbnkMember left;
    std::optional<CurrentSbnkMember> right;
    CurrentSbnkMember inactive_right;
    std::array<std::uint32_t, 4> linked_program_bitmap_words{};
    std::vector<std::uint8_t> linked_program_numbers;
    std::uint8_t sample_flags{};
    std::uint8_t mapout_flags{};
    std::uint8_t key_range_high{};
    std::uint8_t key_range_low{};
    std::uint8_t sample_level{};
    std::int8_t pan{};
    std::uint8_t velocity_range_high{};
    std::uint8_t velocity_range_low{};
    std::vector<SbnkControlRecord> control_records;
    std::vector<NumericField> numeric_fields;
    std::vector<std::byte> raw_parameter_window;

    [[nodiscard]] const NumericField *find_numeric_field(std::string_view name) const noexcept;
};

struct SbacSlot {
    std::string name;
    std::uint32_t raw_handle{};
    std::uint32_t offset{};
};

struct CurrentSbac {
    std::array<std::byte, 0xe0> raw_sample_parameter_block{};
    std::array<std::uint32_t, 3> value_enable_words{};
    std::vector<std::uint8_t> enabled_parameter_numbers;
    std::vector<std::uint8_t> enabled_numbers_outside_table;
    std::uint8_t bulk_assigned_sample_count{};
    std::uint8_t active_slot_count{};
    std::size_t maximum_slot_count{};
    std::vector<SbacSlot> slots;
};

struct ProgAssignment {
    std::string name;
    std::uint32_t raw_handle{};
    std::uint8_t kind{};
    std::uint8_t flags{};
    std::int8_t level_offset{};
    std::int8_t velocity_sensitivity{};
    std::int8_t pan_offset{};
    std::uint8_t key_limit_high{};
    std::uint8_t key_limit_low{};
    std::uint8_t velocity_limit_high{};
    std::uint8_t velocity_limit_low{};
    std::array<std::byte, 0x38> raw_row{};
};

struct CurrentProg {
    std::string program_name;
    std::vector<SbnkControlRecord> control_records;
    std::vector<std::byte> raw_control_block;
    std::vector<std::byte> raw_control_tail_copy;
    std::array<std::vector<std::byte>, 3> effect_blocks;
    std::vector<ProgAssignment> assignments;
};

struct CurrentSequence {
    std::vector<std::byte> raw_payload;
};

struct CurrentProfile {
    std::vector<std::byte> raw_payload;
};

using DecodedPayload =
    std::variant<GenericObject, CurrentSmpl, CurrentSbnk, CurrentSbac, CurrentProg, CurrentSequence, CurrentProfile>;

struct DecodedObject {
    ObjectHeader header;
    ObjectFormat format{ObjectFormat::unknown};
    DecodedPayload payload;
};

AXK_API Result<ObjectHeader> decode_object_header(std::span<const std::byte> payload);
AXK_API Result<DecodedObject> decode_object(std::span<const std::byte> payload);

} // namespace axk
