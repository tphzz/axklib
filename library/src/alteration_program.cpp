#include "alteration_internal.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <tuple>

#include "axklib/bytes.hpp"
#include "axklib/object.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/writer_internal.hpp"

namespace axk::alteration_internal {

Result<OperationReport> delete_program(TransactionState &state, OperationContext context,
                                       const DeleteProgramOperation &operation, const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end() || !operation.program_number) {
        return std::unexpected{transaction_error("delete-program target is invalid")};
    }
    auto &partition = found->second;
    const auto name = std::format("{:03}", operation.program_number);
    auto located = category_object(state, partition, operation.volume_name, "PROG", name, "PROG", cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto payload = current_payload(state, partition, located->second, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto decoded = decode_object(*payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto *program = std::get_if<CurrentProg>(&decoded->payload);
    if (program == nullptr)
        return std::unexpected{transaction_error("Program is unreadable")};
    std::set<SfsId> assigned_samples;
    for (const auto &assignment : program->assignments) {
        if (assignment.name.empty() || assignment.kind != 0x10U)
            continue;
        auto sample =
            category_object(state, partition, operation.volume_name, "SBNK", assignment.name, "SBNK", cancellation);
        if (!sample)
            return std::unexpected{sample.error()};
        assigned_samples.insert(sample->second);
    }
    auto samples = category_objects(state, partition, operation.volume_name, "SBNK", ObjectType::sbnk, cancellation);
    if (!samples)
        return std::unexpected{samples.error()};
    std::set<SfsId> bitmap_samples;
    for (const auto &sample : *samples) {
        auto bit = sbnk_program_bit(sample.payload, operation.program_number);
        if (!bit)
            return std::unexpected{bit.error()};
        if (*bit)
            bitmap_samples.insert(sample.id);
    }
    if (assigned_samples != bitmap_samples) {
        return std::unexpected{transaction_error("Program direct assignments do not match SBNK "
                                                 "Program-link bitmaps")};
    }
    for (const auto id : assigned_samples) {
        if (auto updated = set_sbnk_program_bit(state, partition, id, operation.program_number, false, cancellation);
            !updated)
            return std::unexpected{updated.error()};
    }
    if (auto removed = remove_directory_entry(state, partition, located->first, located->second, name, cancellation);
        !removed)
        return std::unexpected{removed.error()};
    auto freed = release_record(partition, located->second);
    if (!freed)
        return std::unexpected{freed.error()};
    std::erase_if(state.known_edges, [&](const auto &edge) {
        const auto &[edge_partition, source, target] = edge;
        return edge_partition == *partition_index && (source == located->second || target == located->second);
    });
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = name;
    report.removed_sfs_ids = {located->second};
    report.freed_clusters = *freed;
    return report;
}

Result<OperationReport> insert_program(TransactionState &state, OperationContext context,
                                       const InsertProgramOperation &operation, const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end()) {
        return std::unexpected{transaction_error("insert-program target is invalid")};
    }
    auto &partition = found->second;
    const auto &spec = operation.program;
    const auto name = std::format("{:03}", spec.number);
    auto directory = volume_category(state, partition, operation.volume_name, "PROG", cancellation);
    if (!directory)
        return std::unexpected{directory.error()};
    auto directory_payload = current_payload(state, partition, *directory, cancellation);
    if (!directory_payload)
        return std::unexpected{directory_payload.error()};
    auto entries = parse_directory(*directory_payload, *directory);
    if (!entries)
        return std::unexpected{entries.error()};
    if (std::ranges::any_of(*entries, [&](const auto &entry) { return entry.name == name; })) {
        return std::unexpected{transaction_error("volume already contains Program " + name)};
    }
    if (spec.assignments.size() != 2U) {
        return std::unexpected{transaction_error("Program requires exactly two assignments")};
    }
    const auto &sample_bank_assignment = spec.assignments[0];
    const auto &sample_assignment = spec.assignments[1];
    auto sample_bank = category_object(state, partition, operation.volume_name, "SBAC",
                                       sample_bank_assignment.target_name, "SBAC", cancellation);
    if (!sample_bank)
        return std::unexpected{sample_bank.error()};
    auto sample = category_object(state, partition, operation.volume_name, "SBNK", sample_assignment.target_name,
                                  "SBNK", cancellation);
    if (!sample)
        return std::unexpected{sample.error()};
    auto existing_programs =
        category_objects(state, partition, operation.volume_name, "PROG", ObjectType::prog, cancellation);
    if (!existing_programs)
        return std::unexpected{existing_programs.error()};
    for (const auto &existing : *existing_programs) {
        const auto *decoded_program = std::get_if<CurrentProg>(&existing.decoded.payload);
        for (const auto &assignment : decoded_program->assignments) {
            if ((assignment.kind == 0x11U && assignment.name == sample_bank_assignment.target_name) ||
                (assignment.kind == 0x10U && assignment.name == sample_assignment.target_name)) {
                return std::unexpected{transaction_error("Program target is already assigned by another Program")};
            }
        }
    }
    auto sample_payload = current_payload(state, partition, sample->second, cancellation);
    if (!sample_payload)
        return std::unexpected{sample_payload.error()};
    auto bit = sbnk_program_bit(*sample_payload, spec.number);
    if (!bit)
        return std::unexpected{bit.error()};
    if (*bit)
        return std::unexpected{transaction_error("SBNK already links this Program")};
    auto payload = detail::prepare_prog_payload(spec);
    if (!payload)
        return std::unexpected{payload.error()};
    auto allocated = allocate_record(partition, std::move(*payload), PayloadKind::object);
    if (!allocated)
        return std::unexpected{allocated.error()};
    if (auto appended = append_directory_entry(state, partition, *directory, allocated->first, name, cancellation);
        !appended)
        return std::unexpected{appended.error()};
    if (auto updated = set_sbnk_program_bit(state, partition, sample->second, spec.number, true, cancellation);
        !updated)
        return std::unexpected{updated.error()};
    state.known_edges.emplace_back(*partition_index, allocated->first, sample_bank->second);
    state.known_edges.emplace_back(*partition_index, allocated->first, sample->second);
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = name;
    report.inserted_sfs_ids = {allocated->first};
    report.allocated_clusters = allocated->second;
    return report;
}

Result<OperationReport> rename_program(TransactionState &state, OperationContext context,
                                       const RenameProgramOperation &operation, const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end() || operation.program_number == 0U || operation.program_number > 128U)
        return std::unexpected{transaction_error("rename-program target is invalid")};
    auto &partition = found->second;
    const auto slot_name = std::format("{:03}", operation.program_number);
    auto located = category_object(state, partition, operation.volume_name, "PROG", slot_name, "PROG", cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto payload = current_payload(state, partition, located->second, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto decoded = decode_object(*payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto *program = std::get_if<CurrentProg>(&decoded->payload);
    if (program == nullptr || payload->size() < 0x80U)
        return std::unexpected{transaction_error("Program is unreadable")};
    if (program->program_name == operation.new_program_name)
        return std::unexpected{transaction_error("new_program_name must differ")};
    std::fill(payload->begin() + 0x78, payload->begin() + 0x80, std::byte{' '});
    std::ranges::transform(operation.new_program_name, payload->begin() + 0x78,
                           [](char value) { return static_cast<std::byte>(value); });
    if (auto replaced =
            replace_fixed_object_payload(state, partition, located->second, std::move(*payload), cancellation);
        !replaced) {
        return std::unexpected{replaced.error()};
    }
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = operation.new_program_name;
    return report;
}

Result<OperationReport> delete_sbac(TransactionState &state, OperationContext context,
                                    const DeleteSampleBankOperation &operation, const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end()) {
        return std::unexpected{transaction_error("partition index does not exist")};
    }
    auto &partition = found->second;
    auto located = category_object(state, partition, operation.volume_name, "SBAC", operation.sample_bank_name, "SBAC",
                                   cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto payload = current_payload(state, partition, located->second, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto decoded = decode_object(*payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto *sample_bank = std::get_if<CurrentSbac>(&decoded->payload);
    if (sample_bank == nullptr || sample_bank->active_slot_count > sample_bank->maximum_slot_count) {
        return std::unexpected{transaction_error("Sample Bank slots are unreadable")};
    }
    auto programs = category_objects(state, partition, operation.volume_name, "PROG", ObjectType::prog, cancellation);
    if (!programs)
        return std::unexpected{programs.error()};
    for (const auto &program_row : *programs) {
        const auto *program = std::get_if<CurrentProg>(&program_row.decoded.payload);
        if (std::ranges::any_of(program->assignments, [&](const ProgAssignment &assignment) {
                return assignment.kind == 0x11U && assignment.name == operation.sample_bank_name;
            })) {
            return std::unexpected{transaction_error("Sample Bank is referenced by a Program")};
        }
    }
    auto sample_banks =
        category_objects(state, partition, operation.volume_name, "SBAC", ObjectType::sbac, cancellation);
    if (!sample_banks)
        return std::unexpected{sample_banks.error()};
    std::set<std::string> members;
    for (const auto &slot : sample_bank->slots)
        members.insert(slot.name);
    for (const auto &other : *sample_banks) {
        if (other.id == located->second)
            continue;
        const auto *other_sample_bank = std::get_if<CurrentSbac>(&other.decoded.payload);
        for (const auto &slot : other_sample_bank->slots) {
            if (members.contains(slot.name)) {
                return std::unexpected{transaction_error("another Sample Bank shares a Sample")};
            }
        }
    }
    for (const auto &slot : sample_bank->slots) {
        auto sample = category_object(state, partition, operation.volume_name, "SBNK", slot.name, "SBNK", cancellation);
        if (!sample)
            return std::unexpected{sample.error()};
        auto sample_payload = current_payload(state, partition, sample->second, cancellation);
        if (!sample_payload)
            return std::unexpected{sample_payload.error()};
        if (sample_payload->size() <= 0xd0U || (std::to_integer<std::uint8_t>((*sample_payload)[0xd0U]) & 1U) == 0U) {
            return std::unexpected{transaction_error("member Sample is missing its Sample Bank membership flag")};
        }
        if (auto updated = set_sbnk_sample_bank_flag(state, partition, sample->second, false, cancellation); !updated)
            return std::unexpected{updated.error()};
    }
    if (auto removed = remove_directory_entry(state, partition, located->first, located->second,
                                              operation.sample_bank_name, cancellation);
        !removed)
        return std::unexpected{removed.error()};
    auto freed = release_record(partition, located->second);
    if (!freed)
        return std::unexpected{freed.error()};
    std::erase_if(state.known_edges, [&](const auto &edge) {
        const auto &[edge_partition, source, target] = edge;
        return edge_partition == *partition_index && (source == located->second || target == located->second);
    });
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = operation.sample_bank_name;
    report.removed_sfs_ids = {located->second};
    report.freed_clusters = *freed;
    return report;
}

Result<OperationReport> insert_sbac(TransactionState &state, OperationContext context,
                                    const InsertSampleBankOperation &operation, const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end()) {
        return std::unexpected{transaction_error("insert-sbac target is invalid")};
    }
    auto &partition = found->second;
    const auto &spec = operation.sample_bank;
    auto directory = volume_category(state, partition, operation.volume_name, "SBAC", cancellation);
    if (!directory)
        return std::unexpected{directory.error()};
    auto existing_sample_banks =
        category_objects(state, partition, operation.volume_name, "SBAC", ObjectType::sbac, cancellation);
    if (!existing_sample_banks)
        return std::unexpected{existing_sample_banks.error()};
    std::set<std::string> existing_members;
    for (const auto &existing : *existing_sample_banks) {
        if (existing.name == spec.name) {
            return std::unexpected{transaction_error("Sample Bank already exists")};
        }
        const auto *sample_bank = std::get_if<CurrentSbac>(&existing.decoded.payload);
        for (const auto &slot : sample_bank->slots)
            existing_members.insert(slot.name);
    }
    std::map<std::string, SampleSpec> sample_specs;
    std::vector<SfsId> member_ids;
    for (const auto &name : spec.member_samples) {
        if (existing_members.contains(name)) {
            return std::unexpected{transaction_error("Sample already belongs to another Sample Bank")};
        }
        auto sample = category_object(state, partition, operation.volume_name, "SBNK", name, "SBNK", cancellation);
        if (!sample)
            return std::unexpected{sample.error()};
        auto sample_payload = current_payload(state, partition, sample->second, cancellation);
        if (!sample_payload)
            return std::unexpected{sample_payload.error()};
        auto decoded = decode_object(*sample_payload);
        if (!decoded)
            return std::unexpected{decoded.error()};
        const auto *current_sample = std::get_if<CurrentSbnk>(&decoded->payload);
        if (current_sample == nullptr || current_sample->right_slot_present ||
            (std::to_integer<std::uint8_t>((*sample_payload)[0xd0U]) & 1U) != 0U) {
            return std::unexpected{
                transaction_error("Sample Bank profile requires mono Samples without existing membership")};
        }
        SampleSpec placeholder;
        placeholder.name = name;
        sample_specs.emplace(name, std::move(placeholder));
        member_ids.push_back(sample->second);
    }
    auto payload = detail::prepare_sbac_payload(spec, sample_specs);
    if (!payload)
        return std::unexpected{payload.error()};
    auto allocated = allocate_record(partition, std::move(*payload), PayloadKind::object);
    if (!allocated)
        return std::unexpected{allocated.error()};
    for (const auto id : member_ids) {
        if (auto updated = set_sbnk_sample_bank_flag(state, partition, id, true, cancellation); !updated)
            return std::unexpected{updated.error()};
        state.known_edges.emplace_back(*partition_index, allocated->first, id);
    }
    if (auto appended = append_directory_entry(state, partition, *directory, allocated->first, spec.name, cancellation);
        !appended)
        return std::unexpected{appended.error()};
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = spec.name;
    report.inserted_sfs_ids = {allocated->first};
    report.allocated_clusters = allocated->second;
    return report;
}

Result<OperationReport> rename_sbac(TransactionState &state, OperationContext context,
                                    const RenameSampleBankOperation &operation, const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("partition index does not exist")};
    auto &partition = found->second;
    auto located = category_object(state, partition, operation.volume_name, "SBAC", operation.sample_bank_name, "SBAC",
                                   cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto sample_banks =
        category_objects(state, partition, operation.volume_name, "SBAC", ObjectType::sbac, cancellation);
    if (!sample_banks)
        return std::unexpected{sample_banks.error()};
    if (std::ranges::any_of(*sample_banks, [&](const auto &sample_bank) {
            return sample_bank.id != located->second && sample_bank.name == operation.new_sample_bank_name;
        }))
        return std::unexpected{transaction_error("Sample Bank rename destination exists")};
    auto sample_bank_payload = current_payload(state, partition, located->second, cancellation);
    if (!sample_bank_payload)
        return std::unexpected{sample_bank_payload.error()};
    auto sample_bank_object = decode_object(*sample_bank_payload);
    if (!sample_bank_object)
        return std::unexpected{sample_bank_object.error()};
    const auto *sample_bank = std::get_if<CurrentSbac>(&sample_bank_object->payload);
    if (sample_bank == nullptr || sample_bank->slots.empty() ||
        sample_bank->slots.size() != sample_bank->active_slot_count) {
        return std::unexpected{transaction_error("SBAC rename requires a nonempty fully readable slot table")};
    }
    std::set<SfsId> member_ids;
    for (const auto &slot : sample_bank->slots) {
        auto member = category_object(state, partition, operation.volume_name, "SBNK", slot.name, "SBNK", cancellation);
        if (!member)
            return std::unexpected{member.error()};
        auto member_payload = current_payload(state, partition, member->second, cancellation);
        if (!member_payload)
            return std::unexpected{member_payload.error()};
        if (member_payload->size() <= 0xd0U || (std::to_integer<std::uint8_t>((*member_payload)[0xd0U]) & 1U) == 0U) {
            return std::unexpected{transaction_error("Sample Bank member is missing its membership flag")};
        }
        member_ids.insert(member->second);
    }
    for (const auto &other : *sample_banks) {
        if (other.id == located->second)
            continue;
        const auto *other_sample_bank = std::get_if<CurrentSbac>(&other.decoded.payload);
        if (std::ranges::any_of(other_sample_bank->slots, [&](const SbacSlot &slot) {
                return std::ranges::any_of(sample_bank->slots,
                                           [&](const SbacSlot &own) { return own.name == slot.name; });
            })) {
            return std::unexpected{transaction_error("another SBAC shares a rename member")};
        }
    }
    std::set<SfsId> known_members;
    for (const auto &[edge_partition, source, target] : state.known_edges) {
        if (edge_partition == *partition_index && source == located->second) {
            known_members.insert(target);
        }
    }
    if (known_members != member_ids) {
        return std::unexpected{transaction_error("SBAC raw members disagree with known edges")};
    }
    auto programs = category_objects(state, partition, operation.volume_name, "PROG", ObjectType::prog, cancellation);
    if (!programs)
        return std::unexpected{programs.error()};
    std::set<SfsId> updated_programs;
    for (const auto &program_row : *programs) {
        const auto *program = std::get_if<CurrentProg>(&program_row.decoded.payload);
        auto payload = program_row.payload;
        ByteWriter writer{payload};
        bool changed{};
        for (std::size_t index = 0; index < program->assignments.size(); ++index) {
            const auto &assignment = program->assignments[index];
            if (assignment.kind != 0x11U)
                continue;
            if (assignment.name == operation.new_sample_bank_name)
                return std::unexpected{transaction_error("Program already assigns rename destination")};
            if (assignment.name != operation.sample_bank_name)
                continue;
            put_padded_name(payload, 0x120U + index * 0x38U, operation.new_sample_bank_name);
            if (auto written = writer.write_be32(0x130U + index * 0x38U, 0U); !written)
                return std::unexpected{written.error()};
            changed = true;
        }
        if (changed) {
            if (auto replaced =
                    replace_fixed_object_payload(state, partition, program_row.id, std::move(payload), cancellation);
                !replaced)
                return std::unexpected{replaced.error()};
            updated_programs.insert(program_row.id);
        }
    }
    std::set<SfsId> known_programs;
    for (const auto &[edge_partition, source, target] : state.known_edges) {
        if (edge_partition == *partition_index && target == located->second) {
            known_programs.insert(source);
        }
    }
    if (known_programs != updated_programs) {
        return std::unexpected{transaction_error("SBAC raw Program references disagree with known edges")};
    }
    if (auto renamed = rename_object_payload(state, partition, located->second, operation.sample_bank_name,
                                             operation.new_sample_bank_name, cancellation);
        !renamed)
        return std::unexpected{renamed.error()};
    if (auto renamed = rename_directory_entry(state, partition, located->first, located->second,
                                              operation.sample_bank_name, operation.new_sample_bank_name, cancellation);
        !renamed)
        return std::unexpected{renamed.error()};
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = operation.new_sample_bank_name;
    return report;
}

} // namespace axk::alteration_internal
