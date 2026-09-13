#include "axklib/system_file.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace axk {
namespace {

constexpr std::size_t a3000_system_body_size = 0x400U;
constexpr std::size_t system2_body_size = 0x1000U;
constexpr std::size_t system_header_size = 0x20U;
constexpr std::size_t a3000_system_bulk_size = 0x348U;
constexpr std::size_t storage_revision_offset = 0x0eU;
constexpr std::size_t system_parameter_offset = system_header_size;
constexpr std::size_t basic_receive_offset = system_parameter_offset + 20U;
constexpr std::size_t receive_flags_offset = system_parameter_offset + 22U;
constexpr std::size_t program_mode_offset = system_parameter_offset + 62U;
constexpr std::size_t multi_part_offset = system_parameter_offset + 64U;
constexpr std::array<std::byte, 4> a3000_system_magic{std::byte{0x21}, std::byte{0x52}, std::byte{0x05},
                                                      std::byte{0x31}};
constexpr std::array<std::byte, 4> system2_magic{std::byte{0xde}, std::byte{0xad}, std::byte{0xfa}, std::byte{0xce}};

std::string_view system_file_name(SystemFileKind kind) {
    switch (kind) {
    case SystemFileKind::a3000_system:
        return "SYSTEM";
    case SystemFileKind::a4000_a5000_system2:
        return "SYSTEM2";
    }
    return "SYSTEM";
}

Error malformed_system_file(SystemFileKind kind, std::string message) {
    ErrorContext context;
    context.object_type = std::string{system_file_name(kind)};
    return make_error(ErrorCode::object_malformed, ErrorCategory::object, std::move(message), std::move(context));
}

Error invalid_system_path(const Partition &partition, SystemFileKind kind, ErrorCode code, std::string message) {
    ErrorContext context;
    context.partition_index = partition.index.value;
    context.object_type = std::string{system_file_name(kind)};
    return make_error(code, ErrorCategory::relationship, std::move(message), std::move(context));
}

std::uint8_t byte_value(std::span<const std::byte> payload, std::size_t offset) {
    return std::to_integer<std::uint8_t>(payload[offset]);
}

SystemMidiAddress midi_address(std::uint8_t index) {
    if (index < 16U)
        return {.port = MidiPort::a, .channel = static_cast<std::uint8_t>(index + 1U)};
    return {.port = MidiPort::b, .channel = static_cast<std::uint8_t>(index - 15U)};
}

std::vector<std::byte> copy_bytes(std::span<const std::byte> payload, std::size_t offset, std::size_t size) {
    const auto section = payload.subspan(offset, size);
    return {section.begin(), section.end()};
}

Result<DecodedSystemFile> decode_a3000_system(const CurrentRecordEnvelope &envelope,
                                              std::span<const std::byte> payload) {
    constexpr auto kind = SystemFileKind::a3000_system;
    if (payload.size() != a3000_system_body_size) {
        return std::unexpected{malformed_system_file(kind, std::format("SYSTEM body must be exactly {} bytes, found {}",
                                                                       a3000_system_body_size, payload.size()))};
    }
    if (!std::ranges::equal(payload.first(a3000_system_magic.size()), a3000_system_magic))
        return std::unexpected{malformed_system_file(kind, "SYSTEM has an invalid file signature")};
    if (byte_value(payload, storage_revision_offset) != 0U)
        return std::unexpected{malformed_system_file(kind, "SYSTEM has an unsupported storage revision")};

    const auto raw_basic_receive = byte_value(payload, basic_receive_offset);

    const auto receive_flags = byte_value(payload, receive_flags_offset);
    DecodedSystemFile result;
    result.kind = kind;
    result.storage_revision = 0U;
    result.record_envelope = envelope;
    result.system_header_bytes = copy_bytes(payload, 0U, system_header_size);
    result.system_bulk_bytes = copy_bytes(payload, system_header_size, a3000_system_bulk_size);
    result.reserved_tail_bytes = copy_bytes(payload, system_header_size + a3000_system_bulk_size,
                                            a3000_system_body_size - system_header_size - a3000_system_bulk_size);
    result.context = A3000SystemContext{
        .basic_receive = raw_basic_receive < 16U ? std::optional{midi_address(raw_basic_receive)} : std::nullopt,
        .omni = (receive_flags & 0x01U) != 0U,
        .program_change_enabled = (receive_flags & 0x02U) != 0U,
    };
    return result;
}

Result<DecodedSystemFile> decode_system2(const CurrentRecordEnvelope &envelope, std::span<const std::byte> payload) {
    constexpr auto kind = SystemFileKind::a4000_a5000_system2;
    if (payload.size() != system2_body_size) {
        return std::unexpected{malformed_system_file(
            kind, std::format("SYSTEM2 body must be exactly {} bytes, found {}", system2_body_size, payload.size()))};
    }
    if (!std::ranges::equal(payload.first(system2_magic.size()), system2_magic))
        return std::unexpected{malformed_system_file(kind, "SYSTEM2 has an invalid file signature")};

    const auto storage_revision = byte_value(payload, storage_revision_offset);
    if (storage_revision > 1U)
        return std::unexpected{malformed_system_file(kind, "SYSTEM2 has an unsupported storage revision")};

    std::optional<ProgramMode> mode;
    const auto raw_mode = byte_value(payload, program_mode_offset);
    if (raw_mode == 0U)
        mode = ProgramMode::single;
    else if (raw_mode == 1U)
        mode = ProgramMode::multi;

    const auto raw_basic_receive = byte_value(payload, basic_receive_offset);
    constexpr auto part_count = 32U;

    A4000A5000SystemContext context;
    context.saved_program_mode = mode;
    if (raw_basic_receive < part_count)
        context.basic_receive = midi_address(raw_basic_receive);
    const auto receive_flags = byte_value(payload, receive_flags_offset);
    context.omni = (receive_flags & 0x01U) != 0U;
    context.program_change_enabled = (receive_flags & 0x02U) != 0U;
    context.parts.reserve(part_count);
    for (std::size_t index = 0U; index < part_count; ++index) {
        const auto part_index = static_cast<std::uint8_t>(index);
        const auto raw_program = byte_value(payload, multi_part_offset + index);
        context.parts.push_back({
            .part_number = static_cast<std::uint8_t>(index + 1U),
            .midi = midi_address(part_index),
            .program_number =
                raw_program >= 1U && raw_program <= 128U ? std::optional<std::uint16_t>{raw_program} : std::nullopt,
            .master = context.basic_receive ? std::optional{part_index == raw_basic_receive} : std::nullopt,
        });
    }

    DecodedSystemFile result;
    result.kind = kind;
    result.storage_revision = storage_revision;
    result.record_envelope = envelope;
    result.system_header_bytes = copy_bytes(payload, 0U, system_header_size);
    result.system_bulk_bytes = copy_bytes(payload, system_header_size, system2_body_size - system_header_size);
    result.context = std::move(context);
    return result;
}

std::vector<const IndexRecord *> directories_with_id(const Partition &partition, LinkId id) {
    std::vector<const IndexRecord *> result;
    for (const auto &record : partition.records) {
        if (record.payload_kind == PayloadKind::directory && record.directory_id && *record.directory_id == id)
            result.push_back(&record);
    }
    return result;
}

std::vector<const IndexRecord *> records_with_id(const Partition &partition, SfsId id) {
    std::vector<const IndexRecord *> result;
    for (const auto &record : partition.records) {
        if (record.sfs_id == id)
            result.push_back(&record);
    }
    return result;
}

std::vector<const DirectoryEntry *> named_entries(const IndexRecord &directory, std::string_view name) {
    std::vector<const DirectoryEntry *> result;
    for (const auto &entry : directory.directory_entries) {
        if (entry.name == name && entry.target_link_id)
            result.push_back(&entry);
    }
    return result;
}

} // namespace

std::size_t system_file_record_size(SystemFileKind kind) noexcept {
    return current_record_envelope_size +
           (kind == SystemFileKind::a3000_system ? a3000_system_body_size : system2_body_size);
}

Result<DecodedSystemFile> decode_system_file(SystemFileKind kind, std::span<const std::byte> payload) {
    const auto expected_size = system_file_record_size(kind);
    if (payload.size() != expected_size) {
        return std::unexpected{
            malformed_system_file(kind, std::format("{} logical record must be exactly {} bytes, found {}",
                                                    system_file_name(kind), expected_size, payload.size()))};
    }
    const auto envelope = decode_current_record_envelope(payload);
    if (!envelope)
        return std::unexpected{envelope.error()};
    if (envelope->type != ObjectType::prf3) {
        return std::unexpected{
            malformed_system_file(kind, std::format("{} record does not use the PRF3 type", system_file_name(kind)))};
    }
    const auto body = payload.subspan(current_record_envelope_size);
    switch (kind) {
    case SystemFileKind::a3000_system:
        return decode_a3000_system(*envelope, body);
    case SystemFileKind::a4000_a5000_system2:
        return decode_system2(*envelope, body);
    }
    return std::unexpected{malformed_system_file(kind, "unsupported System File kind")};
}

Result<std::vector<std::byte>> encode_system_file(const DecodedSystemFile &file) {
    if (file.kind != SystemFileKind::a3000_system && file.kind != SystemFileKind::a4000_a5000_system2)
        return std::unexpected{malformed_system_file(file.kind, "unsupported System File kind")};
    const auto native = file.kind == SystemFileKind::a3000_system;
    const auto bulk_size = native ? a3000_system_bulk_size : system2_body_size - system_header_size;
    const auto tail_size = native ? a3000_system_body_size - system_header_size - a3000_system_bulk_size : 0U;
    if (file.system_header_bytes.size() != system_header_size || file.system_bulk_bytes.size() != bulk_size ||
        file.reserved_tail_bytes.size() != tail_size)
        return std::unexpected{malformed_system_file(file.kind, "System File retained sections have invalid sizes")};

    std::vector<std::byte> bytes;
    bytes.reserve(system_file_record_size(file.kind));
    bytes.insert(bytes.end(), file.record_envelope.raw_bytes.begin(), file.record_envelope.raw_bytes.end());
    bytes.insert(bytes.end(), file.system_header_bytes.begin(), file.system_header_bytes.end());
    bytes.insert(bytes.end(), file.system_bulk_bytes.begin(), file.system_bulk_bytes.end());
    bytes.insert(bytes.end(), file.reserved_tail_bytes.begin(), file.reserved_tail_bytes.end());
    const auto decoded = decode_system_file(file.kind, bytes);
    if (!decoded)
        return std::unexpected{decoded.error()};
    if (*decoded != file)
        return std::unexpected{
            malformed_system_file(file.kind, "System File metadata does not match its retained bytes")};
    return bytes;
}

Result<std::optional<SfsId>> locate_system_file_record(const Partition &partition, SystemFileKind kind) {
    const auto root_id = locate_partition_root_record(partition);
    if (!root_id)
        return std::unexpected{root_id.error()};
    const auto roots = records_with_id(partition, *root_id);
    if (roots.size() != 1U || roots.front()->payload_kind != PayloadKind::directory) {
        return std::unexpected{invalid_system_path(partition, kind, ErrorCode::relationship_unresolved,
                                                   "partition root record is unavailable")};
    }
    const auto *root = roots.front();
    const auto prf3_entries = named_entries(*root, "PRF3");
    if (prf3_entries.empty())
        return std::optional<SfsId>{};
    if (prf3_entries.size() != 1U) {
        return std::unexpected{invalid_system_path(partition, kind, ErrorCode::relationship_ambiguous,
                                                   "partition root has multiple PRF3 entries")};
    }

    const auto prf3_directories = directories_with_id(partition, *prf3_entries.front()->target_link_id);
    if (prf3_directories.size() != 1U || !prf3_directories.front()->parent_directory_id || !root->directory_id ||
        *prf3_directories.front()->parent_directory_id != *root->directory_id) {
        return std::unexpected{invalid_system_path(
            partition, kind,
            prf3_directories.size() > 1U ? ErrorCode::relationship_ambiguous : ErrorCode::relationship_unresolved,
            "partition root PRF3 entry does not resolve to one direct child directory")};
    }

    const auto file_name = system_file_name(kind);
    const auto system_entries = named_entries(*prf3_directories.front(), file_name);
    if (system_entries.empty())
        return std::optional<SfsId>{};
    if (system_entries.size() != 1U) {
        return std::unexpected{
            invalid_system_path(partition, kind, ErrorCode::relationship_ambiguous,
                                std::format("partition PRF3 directory has multiple {} entries", file_name))};
    }

    const auto records = records_with_id(partition, SfsId{system_entries.front()->target_link_id->value});
    if (records.size() != 1U || records.front()->payload_kind == PayloadKind::directory) {
        return std::unexpected{invalid_system_path(
            partition, kind,
            records.size() > 1U ? ErrorCode::relationship_ambiguous : ErrorCode::relationship_unresolved,
            std::format("partition PRF3/{} entry does not resolve to one file record", file_name))};
    }
    return std::optional<SfsId>{records.front()->sfs_id};
}

} // namespace axk
