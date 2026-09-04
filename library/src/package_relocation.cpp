#include "axklib/package_relocation.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <set>
#include <utility>

#include "axklib/bytes.hpp"

namespace axk::package_internal {
namespace {

Error profile_error(const DecodedObject &object, std::string message) {
    ErrorContext context;
    context.object_type = object.header.raw_type;
    context.object_name = object.header.name;
    return make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported, std::move(message),
                      std::move(context));
}

std::string hex_bytes(std::span<const std::byte> bytes) {
    std::string result;
    result.reserve(bytes.size() * 2U);
    for (const auto value : bytes)
        result += std::format("{:02x}", std::to_integer<std::uint8_t>(value));
    return result;
}

Result<void> add_range(RelocationProfile &profile, const DecodedObject &object, std::uint32_t offset,
                       std::uint32_t width, std::string role, std::span<const std::byte> mask = {}) {
    if (offset > profile.normalized_payload.size() || width > profile.normalized_payload.size() - offset ||
        (!mask.empty() && mask.size() != width)) {
        return std::unexpected{profile_error(object, "object relocation range is out of bounds")};
    }
    const auto source = std::span<const std::byte>{profile.normalized_payload}.subspan(offset, width);
    PackageRelocation relocation;
    relocation.offset = offset;
    relocation.width = width;
    relocation.role = std::move(role);
    relocation.expected_hex = hex_bytes(source);
    relocation.mask_hex = hex_bytes(mask);
    profile.relocations.push_back(std::move(relocation));
    for (std::size_t index = 0; index < width; ++index) {
        if (mask.empty()) {
            profile.normalized_payload[offset + index] = std::byte{};
        } else {
            profile.normalized_payload[offset + index] &= ~mask[index];
        }
    }
    return {};
}

Error relocation_error(const PackageNode &node, std::string message) {
    ErrorContext context;
    context.object_type = node.object_type;
    context.object_name = node.name;
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message),
                      std::move(context));
}

Result<void> put_name(std::span<std::byte> payload, std::size_t offset, std::string_view name) {
    if (name.empty() || name.size() > 16U ||
        !std::ranges::all_of(name, [](unsigned char value) { return value < 0x80U; }) || offset > payload.size() ||
        16U > payload.size() - offset) {
        return std::unexpected{make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction,
                                          "package relocation name is invalid or out of bounds")};
    }
    std::fill(payload.begin() + static_cast<std::ptrdiff_t>(offset),
              payload.begin() + static_cast<std::ptrdiff_t>(offset + 16U), std::byte{' '});
    std::ranges::transform(name, payload.begin() + static_cast<std::ptrdiff_t>(offset),
                           [](char value) { return static_cast<std::byte>(value); });
    return {};
}

Result<std::string_view> target_name(const PackageNode &node, const PackageNodeRelocationContext &context,
                                     const PackageRelationship &edge) {
    const auto found = context.edge_target_names.find(edge.edge_id);
    if (found == context.edge_target_names.end()) {
        return std::unexpected{relocation_error(node, "package relocation is missing an edge target name")};
    }
    return found->second;
}

Result<std::uint32_t> target_wave_data_reference_value(const PackageNode &node,
                                                       const PackageNodeRelocationContext &context,
                                                       const PackageRelationship &edge) {
    const auto found = context.edge_target_reference_values.find(edge.edge_id);
    if (found == context.edge_target_reference_values.end()) {
        return std::unexpected{relocation_error(node, "package relocation is missing an SMPL target reference value")};
    }
    return found->second;
}

struct ByteRange {
    std::size_t offset{};
    std::size_t size{};
};

bool contains(std::span<const ByteRange> ranges, std::size_t offset) {
    return std::ranges::any_of(
        ranges, [=](const ByteRange &range) { return offset >= range.offset && offset - range.offset < range.size; });
}

Result<void> clear_program_assignment_rows_in_place(std::vector<std::byte> &payload, const CurrentProg &program,
                                                    std::span<const std::uint32_t> ordinals,
                                                    std::vector<ByteRange> *changed_ranges) {
    std::set<std::uint32_t> unique;
    for (const auto ordinal : ordinals) {
        if (ordinal >= program.assignments.size() || !unique.emplace(ordinal).second) {
            return std::unexpected{make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction,
                                              "Program assignment adjustment ordinal is invalid or duplicated")};
        }
        const auto &assignment = program.assignments[ordinal];
        if (assignment.name.empty() || (assignment.kind != 0x10U && assignment.kind != 0x11U)) {
            return std::unexpected{make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction,
                                              "Program assignment adjustment does not name a stored assignment row")};
        }
        const auto offset = 0x120U + static_cast<std::size_t>(ordinal) * 0x38U;
        if (offset > payload.size() || 0x38U > payload.size() - offset) {
            return std::unexpected{make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction,
                                              "Program assignment adjustment row is out of bounds")};
        }
        std::fill(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                  payload.begin() + static_cast<std::ptrdiff_t>(offset + 0x38U), std::byte{});
        if (changed_ranges != nullptr)
            changed_ranges->push_back({offset, 0x38U});
    }
    return {};
}

Result<std::vector<std::byte>> patch_names(const PortablePackage &package, const PackageNode &node,
                                           const PackageNodeRelocationContext &context,
                                           std::vector<ByteRange> *changed_ranges) {
    auto result = node.raw_payload;
    auto decoded = decode_package_object(node.raw_payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    if (decoded->header.name != context.destination_name) {
        if (auto patched = put_name(result, 0x32U, context.destination_name); !patched)
            return std::unexpected{patched.error()};
        changed_ranges->push_back({0x32U, 16U});
    }
    if (!context.cleared_program_assignment_ordinals.empty()) {
        const auto *program = std::get_if<CurrentProg>(&decoded->payload);
        if (program == nullptr) {
            return std::unexpected{relocation_error(node, "Program assignment adjustment source is not a Program")};
        }
        if (auto cleared = clear_program_assignment_rows_in_place(
                result, *program, context.cleared_program_assignment_ordinals, changed_ranges);
            !cleared) {
            return std::unexpected{cleared.error()};
        }
    }

    for (const auto &edge : package.relationships) {
        if (edge.source_node_id != node.node_id)
            continue;
        const auto name = target_name(node, context, edge);
        if (!name)
            return std::unexpected{name.error()};
        std::size_t offset{};
        std::string_view source_name;
        if (edge.role == "SBNK_LEFT_MEMBER_TO_SMPL") {
            offset = 0x78U;
            const auto *sample = std::get_if<CurrentSbnk>(&decoded->payload);
            if (sample == nullptr)
                return std::unexpected{relocation_error(node, "SBNK relationship source is not decoded")};
            source_name = sample->left.wave_data_name;
        } else if (edge.role == "SBNK_RIGHT_MEMBER_TO_SMPL") {
            offset = 0x88U;
            const auto *sample = std::get_if<CurrentSbnk>(&decoded->payload);
            if (sample == nullptr || !sample->right)
                return std::unexpected{relocation_error(node, "SBNK right relationship is not decoded")};
            source_name = sample->right->wave_data_name;
        } else if (edge.role == "SBAC_SLOT_TO_SBNK") {
            offset = 0x14cU + static_cast<std::size_t>(edge.ordinal) * 0x14U;
            const auto *sample_bank = std::get_if<CurrentSbac>(&decoded->payload);
            if (sample_bank == nullptr || edge.ordinal >= sample_bank->slots.size())
                return std::unexpected{relocation_error(node, "SBAC relationship source is not decoded")};
            source_name = sample_bank->slots[edge.ordinal].name;
        } else if (edge.role == "PROG_ASSIGNMENT_TO_SBAC" || edge.role == "PROG_ASSIGNMENT_TO_SBNK") {
            offset = 0x120U + static_cast<std::size_t>(edge.ordinal) * 0x38U;
            const auto *program = std::get_if<CurrentProg>(&decoded->payload);
            if (program == nullptr || edge.ordinal >= program->assignments.size()) {
                return std::unexpected{relocation_error(node, "Program relationship source is not decoded")};
            }
            source_name = program->assignments[edge.ordinal].name;
        } else {
            return std::unexpected{relocation_error(node, "package relocation encountered an unsupported edge role")};
        }
        if (source_name != *name) {
            if (auto patched = put_name(result, offset, *name); !patched)
                return std::unexpected{patched.error()};
            changed_ranges->push_back({offset, 16U});
        }
    }
    return result;
}

} // namespace

Result<std::vector<std::byte>> clear_program_assignment_rows(std::span<const std::byte> payload,
                                                             std::span<const std::uint32_t> ordinals) {
    auto decoded = decode_object(payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto *program = std::get_if<CurrentProg>(&decoded->payload);
    if (program == nullptr) {
        return std::unexpected{make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction,
                                          "Program assignment adjustment payload is not a Program")};
    }
    std::vector<std::byte> result(payload.begin(), payload.end());
    if (auto cleared = clear_program_assignment_rows_in_place(result, *program, ordinals, nullptr); !cleared)
        return std::unexpected{cleared.error()};
    auto verified = decode_object(result);
    if (!verified)
        return std::unexpected{verified.error()};
    const auto *verified_program = std::get_if<CurrentProg>(&verified->payload);
    if (verified_program == nullptr) {
        return std::unexpected{make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction,
                                          "adjusted Program payload did not decode as a Program")};
    }
    for (const auto ordinal : ordinals) {
        if (verified_program->assignments[ordinal].raw_row != std::array<std::byte, 0x38>{}) {
            return std::unexpected{make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction,
                                              "adjusted Program row did not decode as empty")};
        }
    }
    return result;
}

Result<RelocationProfile> build_relocation_profile(const DecodedObject &object,
                                                   std::span<const std::byte> raw_payload) {
    if (object.format != ObjectFormat::current) {
        if (is_opaque_sequence(object))
            return RelocationProfile{{raw_payload.begin(), raw_payload.end()}, {}};
        return std::unexpected{profile_error(object, "portable packages require a current Yamaha object profile")};
    }
    RelocationProfile result;
    result.normalized_payload.assign(raw_payload.begin(), raw_payload.end());
    switch (object.header.type) {
    case ObjectType::smpl: {
        const auto *wave_data = std::get_if<CurrentSmpl>(&object.payload);
        if (wave_data == nullptr)
            return std::unexpected{profile_error(object, "current SMPL payload is not decoded")};
        if (wave_data->pcm_transfer_control.value != 0x30U) {
            return std::unexpected{
                profile_error(object, "portable packages require the proven current SMPL PCM transfer profile")};
        }
        if (auto added = add_range(result, object, 0x43U, 7U, "SMPL_SAVE_BUFFER_RESIDUE_0X43"); !added)
            return std::unexpected{added.error()};
        if (auto added = add_range(result, object, 0x54U, 16U, "SMPL_EMBEDDED_CONTAINER_NAME"); !added)
            return std::unexpected{added.error()};
        if (auto added = add_range(result, object, 0x68U, 4U, "SMPL_NAME_HASH_NEXT_HANDLE_ALIAS"); !added)
            return std::unexpected{added.error()};
        if (auto added = add_range(result, object, 0x6cU, 3U, "SMPL_REFERENCE_PREFIX"); !added)
            return std::unexpected{added.error()};
        if (auto added = add_range(result, object, 0x6fU, 5U, "SMPL_SAVE_BUFFER_RESIDUE_0X6F"); !added)
            return std::unexpected{added.error()};
        if (auto added = add_range(result, object, 0x74U, 4U, "SMPL_NAME_HASH_NEXT_HANDLE"); !added)
            return std::unexpected{added.error()};
        if (auto added = add_range(result, object, 0x78U, 4U, "SMPL_REFERENCE_VALUE"); !added)
            return std::unexpected{added.error()};
        if (auto added = add_range(result, object, 0xaaU, 2U, "SMPL_TRANSIENT_512_BYTE_BLOCK_COUNTER"); !added)
            return std::unexpected{added.error()};
        break;
    }
    case ObjectType::sbnk: {
        if (!std::holds_alternative<CurrentSbnk>(object.payload))
            return std::unexpected{profile_error(object, "current SBNK payload is not decoded")};
        if (auto added = add_range(result, object, 0xa0U, 4U, "SBNK_LEFT_MEMBER_CACHED_REFERENCE"); !added)
            return std::unexpected{added.error()};
        if (auto added = add_range(result, object, 0xa4U, 4U, "SBNK_RIGHT_MEMBER_CACHED_REFERENCE"); !added)
            return std::unexpected{added.error()};
        if (auto added = add_range(result, object, 0xc0U, 16U, "SBNK_PROGRAM_BITMAP"); !added)
            return std::unexpected{added.error()};
        constexpr std::array group_mask{std::byte{0x01}};
        if (auto added = add_range(result, object, 0xd0U, 1U, "SBNK_GROUP_MEMBERSHIP", group_mask); !added) {
            return std::unexpected{added.error()};
        }
        break;
    }
    case ObjectType::sbac: {
        const auto *sample_bank = std::get_if<CurrentSbac>(&object.payload);
        if (sample_bank == nullptr)
            return std::unexpected{profile_error(object, "current SBAC payload is not decoded")};
        for (const auto &slot : sample_bank->slots) {
            if (auto added = add_range(result, object, slot.offset + 16U, 4U, "SBAC_SLOT_HANDLE"); !added) {
                return std::unexpected{added.error()};
            }
        }
        break;
    }
    case ObjectType::prog: {
        const auto *program = std::get_if<CurrentProg>(&object.payload);
        if (program == nullptr)
            return std::unexpected{profile_error(object, "current PROG payload is not decoded")};
        for (std::size_t index = 0; index < program->assignments.size(); ++index) {
            const auto &assignment = program->assignments[index];
            const auto supported_kind = assignment.kind == 0x10U || assignment.kind == 0x11U;
            if (!assignment.name.empty() && supported_kind) {
                const auto offset = 0x130U + static_cast<std::uint32_t>(index) * 0x38U;
                if (auto added = add_range(result, object, offset, 4U, "PROG_ASSIGNMENT_HANDLE"); !added)
                    return std::unexpected{added.error()};
            }
        }
        break;
    }
    case ObjectType::sequ:
        if (!std::holds_alternative<CurrentSequence>(object.payload))
            return std::unexpected{profile_error(object, "current SEQU payload is not decoded")};
        break;
    case ObjectType::prf3:
        return std::unexpected{profile_error(object, "PRF3 portability is not admitted for package version 1")};
    case ObjectType::unknown:
        return std::unexpected{profile_error(object, "unknown Yamaha object types are not portable package nodes")};
    }
    return result;
}

Result<std::vector<std::byte>> project_package_node_names(const PortablePackage &package, const PackageNode &node,
                                                          const PackageNodeRelocationContext &context) {
    std::vector<ByteRange> changed_ranges;
    return patch_names(package, node, context, &changed_ranges);
}

Result<std::vector<std::byte>> relocate_package_node(const PortablePackage &package, const PackageNode &node,
                                                     const PackageNodeRelocationContext &context) {
    auto source_decoded = decode_package_object(node.raw_payload);
    if (!source_decoded)
        return std::unexpected{source_decoded.error()};
    auto source_profile = build_relocation_profile(*source_decoded, node.raw_payload);
    if (!source_profile)
        return std::unexpected{source_profile.error()};

    std::vector<ByteRange> allowed;
    auto projected = patch_names(package, node, context, &allowed);
    if (!projected)
        return std::unexpected{projected.error()};
    auto result = *projected;
    ByteWriter writer{result};
    const auto write_be32 = [&](std::size_t offset, std::uint32_t value) -> Result<void> {
        if (auto written = writer.write_be32(offset, value); !written)
            return std::unexpected{relocation_error(node, "package relocation field is out of bounds")};
        return {};
    };
    const auto write_reference_prefix = [&](std::size_t offset, std::uint32_t value) -> Result<void> {
        for (std::size_t index = 0; index < 3U; ++index) {
            const auto shift = static_cast<unsigned int>((3U - index) * 8U);
            if (auto written = writer.write_u8(offset + index, static_cast<std::uint8_t>(value >> shift)); !written) {
                return std::unexpected{relocation_error(node, "package relocation field is out of bounds")};
            }
        }
        return {};
    };
    for (const auto &relocation : node.relocations)
        allowed.push_back({relocation.offset, relocation.width});

    if (node.object_type == "SMPL") {
        if (context.destination_embedded_container_name.empty() || !context.wave_data_reference_value ||
            result.size() < 0xacU) {
            return std::unexpected{
                relocation_error(node, "SMPL relocation requires a destination container and reference value")};
        }
        if (auto written = put_name(result, 0x54U, context.destination_embedded_container_name); !written)
            return std::unexpected{written.error()};
        std::fill(result.begin() + 0x43, result.begin() + 0x4a, std::byte{});
        std::fill(result.begin() + 0x68, result.begin() + 0x6c, std::byte{});
        if (auto written = write_reference_prefix(0x6cU, *context.wave_data_reference_value); !written)
            return std::unexpected{written.error()};
        std::fill(result.begin() + 0x6f, result.begin() + 0x78, std::byte{});
        if (auto written = write_be32(0x78U, *context.wave_data_reference_value); !written)
            return std::unexpected{written.error()};
        std::fill(result.begin() + 0xaa, result.begin() + 0xac, std::byte{});
    } else if (node.object_type == "SBNK") {
        if (result.size() < 0xd1U) {
            return std::unexpected{relocation_error(node, "SBNK relocation payload is truncated")};
        }
        std::optional<std::uint32_t> left_link;
        std::optional<std::uint32_t> right_link;
        for (const auto &edge : package.relationships) {
            if (edge.source_node_id != node.node_id)
                continue;
            if (edge.role == "SBNK_LEFT_MEMBER_TO_SMPL") {
                auto link = target_wave_data_reference_value(node, context, edge);
                if (!link)
                    return std::unexpected{link.error()};
                left_link = *link;
            } else if (edge.role == "SBNK_RIGHT_MEMBER_TO_SMPL") {
                auto link = target_wave_data_reference_value(node, context, edge);
                if (!link)
                    return std::unexpected{link.error()};
                right_link = *link;
            }
        }
        if (!left_link) {
            return std::unexpected{relocation_error(node, "SBNK relocation requires its left SMPL relationship")};
        }
        if (auto written = write_be32(0xa0U, *left_link); !written)
            return std::unexpected{written.error()};
        if (right_link) {
            if (auto written = write_be32(0xa4U, *right_link); !written)
                return std::unexpected{written.error()};
        } else {
            if (auto written = write_be32(0xa4U, 0U); !written)
                return std::unexpected{written.error()};
        }
        std::fill(result.begin() + 0xc0, result.begin() + 0xd0, std::byte{});
        std::set<std::uint8_t> unique_programs;
        for (const auto number : context.linked_program_numbers) {
            if (number < 1U || number > 128U || !unique_programs.emplace(number).second) {
                return std::unexpected{relocation_error(node, "SBNK relocation contains an invalid Program number")};
            }
            const auto word_offset = 0xc0U + static_cast<std::size_t>((number - 1U) / 32U) * 4U;
            const auto bit = std::uint32_t{1} << ((number - 1U) % 32U);
            const auto word = (std::to_integer<std::uint32_t>(result[word_offset]) << 24U) |
                              (std::to_integer<std::uint32_t>(result[word_offset + 1U]) << 16U) |
                              (std::to_integer<std::uint32_t>(result[word_offset + 2U]) << 8U) |
                              std::to_integer<std::uint32_t>(result[word_offset + 3U]);
            if (auto written = write_be32(word_offset, word | bit); !written)
                return std::unexpected{written.error()};
        }
        auto flags = std::to_integer<std::uint8_t>(result[0xd0U]);
        flags = context.sample_bank_member ? static_cast<std::uint8_t>(flags | 1U)
                                           : static_cast<std::uint8_t>(flags & 0xfeU);
        result[0xd0U] = static_cast<std::byte>(flags);
    } else if (node.object_type == "SBAC") {
        const auto *sample_bank = std::get_if<CurrentSbac>(&source_decoded->payload);
        if (sample_bank == nullptr)
            return std::unexpected{relocation_error(node, "SBAC source payload is not decoded")};
        for (const auto &slot : sample_bank->slots) {
            if (auto written = write_be32(slot.offset + 16U, 0U); !written)
                return std::unexpected{written.error()};
        }
    } else if (node.object_type == "PROG") {
        const auto *program = std::get_if<CurrentProg>(&source_decoded->payload);
        if (program == nullptr)
            return std::unexpected{relocation_error(node, "Program source payload is not decoded")};
        for (std::size_t index = 0; index < program->assignments.size(); ++index) {
            const auto &assignment = program->assignments[index];
            if (!assignment.name.empty() && (assignment.kind == 0x10U || assignment.kind == 0x11U)) {
                if (auto written = write_be32(0x130U + index * 0x38U, 0U); !written)
                    return std::unexpected{written.error()};
            }
        }
    } else if (node.object_type == "SEQU") {
        if (!std::holds_alternative<CurrentSequence>(source_decoded->payload) && !is_opaque_sequence(*source_decoded))
            return std::unexpected{relocation_error(node, "Sequence source payload is not decoded")};
    } else {
        return std::unexpected{relocation_error(node, "package node type has no admitted relocation implementation")};
    }

    for (std::size_t offset = 0; offset < result.size(); ++offset) {
        if (result[offset] != node.raw_payload[offset] && !contains(allowed, offset)) {
            return std::unexpected{relocation_error(node, "package relocation changed an undeclared payload byte")};
        }
    }

    auto decoded = decode_package_object(result);
    if (!decoded)
        return std::unexpected{decoded.error()};
    if (decoded->header.name != context.destination_name) {
        return std::unexpected{relocation_error(node, "relocated object header name does not match its destination")};
    }
    if (const auto *wave_data = std::get_if<CurrentSmpl>(&decoded->payload)) {
        if (wave_data->embedded_container_name.value != context.destination_embedded_container_name ||
            wave_data->transient_name_hash_next_handle.value != 0U ||
            wave_data->transient_512_byte_block_counter.value != 0U || !context.wave_data_reference_value ||
            wave_data->wave_data_reference_value.value != *context.wave_data_reference_value) {
            return std::unexpected{
                relocation_error(node, "relocated SMPL metadata did not decode to its canonical destination")};
        }
    } else if (const auto *sample = std::get_if<CurrentSbnk>(&decoded->payload)) {
        std::set<std::uint8_t> expected_programs(context.linked_program_numbers.begin(),
                                                 context.linked_program_numbers.end());
        const std::set<std::uint8_t> actual_programs(sample->linked_program_numbers.begin(),
                                                     sample->linked_program_numbers.end());
        if (expected_programs != actual_programs || ((sample->sample_flags & 1U) != 0U) != context.sample_bank_member)
            return std::unexpected{
                relocation_error(node, "relocated SBNK metadata did not decode to the planned graph")};
        for (const auto &edge : package.relationships) {
            if (edge.source_node_id != node.node_id)
                continue;
            const auto name = target_name(node, context, edge);
            if (!name)
                return std::unexpected{name.error()};
            if (edge.role == "SBNK_LEFT_MEMBER_TO_SMPL") {
                const auto link = target_wave_data_reference_value(node, context, edge);
                if (!link)
                    return std::unexpected{link.error()};
                if (sample->left.wave_data_name != *name || sample->left.cached_wave_data_reference_value != *link) {
                    return std::unexpected{relocation_error(node, "relocated SBNK left member did not decode to "
                                                                  "the planned target")};
                }
            } else if (edge.role == "SBNK_RIGHT_MEMBER_TO_SMPL") {
                const auto link = target_wave_data_reference_value(node, context, edge);
                if (!link)
                    return std::unexpected{link.error()};
                if (!sample->right || sample->right->wave_data_name != *name ||
                    sample->right->cached_wave_data_reference_value != *link) {
                    return std::unexpected{relocation_error(node, "relocated SBNK right member did not decode to "
                                                                  "the planned target")};
                }
            }
        }
    } else if (const auto *sample_bank = std::get_if<CurrentSbac>(&decoded->payload)) {
        for (const auto &edge : package.relationships) {
            if (edge.source_node_id != node.node_id || edge.role != "SBAC_SLOT_TO_SBNK")
                continue;
            const auto name = target_name(node, context, edge);
            if (!name)
                return std::unexpected{name.error()};
            if (edge.ordinal >= sample_bank->slots.size() || sample_bank->slots[edge.ordinal].name != *name ||
                sample_bank->slots[edge.ordinal].transient_member_pointer != 0U) {
                return std::unexpected{relocation_error(node, "relocated SBAC slot did not decode "
                                                              "to the planned target")};
            }
        }
    } else if (const auto *program = std::get_if<CurrentProg>(&decoded->payload)) {
        if (std::ranges::any_of(program->assignments, [](const ProgAssignment &assignment) {
                return !assignment.name.empty() && (assignment.kind == 0x10U || assignment.kind == 0x11U) &&
                       assignment.raw_handle != 0U;
            })) {
            return std::unexpected{relocation_error(node, "relocated Program assignment handle is not zero")};
        }
        for (const auto &edge : package.relationships) {
            if (edge.source_node_id != node.node_id ||
                (edge.role != "PROG_ASSIGNMENT_TO_SBAC" && edge.role != "PROG_ASSIGNMENT_TO_SBNK")) {
                continue;
            }
            const auto name = target_name(node, context, edge);
            if (!name)
                return std::unexpected{name.error()};
            const auto expected_kind = edge.role == "PROG_ASSIGNMENT_TO_SBAC" ? 0x11U : 0x10U;
            if (edge.ordinal >= program->assignments.size() || program->assignments[edge.ordinal].name != *name ||
                program->assignments[edge.ordinal].kind != expected_kind ||
                program->assignments[edge.ordinal].raw_handle != 0U) {
                return std::unexpected{relocation_error(node, "relocated Program assignment did "
                                                              "not decode to the planned target")};
            }
        }
    }

    auto projected_decoded = decode_package_object(*projected);
    if (!projected_decoded)
        return std::unexpected{projected_decoded.error()};
    auto expected_profile = build_relocation_profile(*projected_decoded, *projected);
    auto actual_profile = build_relocation_profile(*decoded, result);
    if (!expected_profile)
        return std::unexpected{expected_profile.error()};
    if (!actual_profile)
        return std::unexpected{actual_profile.error()};
    if (expected_profile->normalized_payload != actual_profile->normalized_payload) {
        return std::unexpected{relocation_error(node, "relocated payload changed normalized identity")};
    }
    return result;
}

} // namespace axk::package_internal
