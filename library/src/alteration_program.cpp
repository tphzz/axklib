#include "alteration_internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <tuple>
#include <vector>

#include "axklib/bytes.hpp"
#include "axklib/object.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/writer_internal.hpp"

namespace axk::alteration_internal {

namespace {

bool ascii_case_equal(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() && std::ranges::equal(left, right, [](unsigned char lhs, unsigned char rhs) {
               return std::tolower(lhs) == std::tolower(rhs);
           });
}

Result<std::map<std::string, std::vector<SfsId>>>
sample_bank_memberships(const std::vector<CategoryObject> &sample_banks) {
    std::map<std::string, std::vector<SfsId>> result;
    for (const auto &row : sample_banks) {
        const auto *sample_bank = std::get_if<CurrentSbac>(&row.decoded.payload);
        if (sample_bank == nullptr || sample_bank->stored_member_count > sample_bank->maximum_member_count ||
            sample_bank->slots.size() != sample_bank->stored_member_count) {
            return std::unexpected{transaction_error("Sample Bank membership is unreadable")};
        }
        for (const auto &slot : sample_bank->slots) {
            if (slot.active)
                result[slot.name].push_back(row.id);
        }
    }
    return result;
}

Result<void> detach_sample_bank_members(TransactionState &state, MutablePartition &partition,
                                        const std::vector<CategoryObject> &sample_banks,
                                        const std::map<std::string, SfsId> &member_ids, PartitionIndex partition_index,
                                        const CancellationToken &cancellation) {
    constexpr std::size_t slot_base = 0x14cU;
    constexpr std::size_t slot_size = 0x14U;
    for (const auto &row : sample_banks) {
        const auto *sample_bank = std::get_if<CurrentSbac>(&row.decoded.payload);
        const auto removed = std::ranges::count_if(
            sample_bank->slots, [&](const SbacSlot &slot) { return member_ids.contains(slot.name); });
        if (removed == 0)
            continue;
        auto payload = row.payload;
        std::size_t write_index{};
        for (const auto &slot : sample_bank->slots) {
            if (member_ids.contains(slot.name))
                continue;
            std::array<std::byte, slot_size> slot_bytes{};
            const auto source = std::span{payload}.subspan(slot.offset, slot_size);
            const auto target = std::span{payload}.subspan(slot_base + write_index * slot_size, slot_size);
            std::ranges::copy(source, slot_bytes.begin());
            std::ranges::copy(slot_bytes, target.begin());
            ++write_index;
        }
        const auto old_slot_end = slot_base + sample_bank->slots.size() * slot_size;
        std::ranges::fill(std::span{payload}.subspan(slot_base + write_index * slot_size,
                                                     old_slot_end - slot_base - write_index * slot_size),
                          std::byte{});
        payload[0x144U] = static_cast<std::byte>(write_index);
        if (auto replaced = replace_fixed_object_payload(state, partition, row.id, std::move(payload), cancellation);
            !replaced) {
            return std::unexpected{replaced.error()};
        }
        std::erase_if(state.known_edges, [&](const auto &edge) {
            const auto &[edge_partition, source, target] = edge;
            return edge_partition == partition_index && source == row.id &&
                   std::ranges::any_of(member_ids, [&](const auto &member) { return member.second == target; });
        });
    }
    return {};
}

} // namespace

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
    if (std::ranges::any_of(*entries, [&](const auto &entry) {
            return entry.state == DirectoryEntryState::live && entry.name == name;
        })) {
        return std::unexpected{transaction_error("volume already contains Program " + name)};
    }
    struct ResolvedTarget {
        const ProgramAssignmentSpec *assignment{};
        SfsId id{};
    };
    std::vector<ResolvedTarget> targets;
    targets.reserve(spec.assignments.size());
    for (const auto &assignment : spec.assignments) {
        const auto category = assignment.target_kind == "SBAC" ? "SBAC" : "SBNK";
        auto target = category_object(state, partition, operation.volume_name, category, assignment.target_name,
                                      category, cancellation);
        if (!target)
            return std::unexpected{target.error()};
        targets.push_back({&assignment, target->second});
    }
    for (const auto &target : targets) {
        if (target.assignment->target_kind != "SBNK")
            continue;
        auto sample_payload = current_payload(state, partition, target.id, cancellation);
        if (!sample_payload)
            return std::unexpected{sample_payload.error()};
        auto bit = sbnk_program_bit(*sample_payload, spec.number);
        if (!bit)
            return std::unexpected{bit.error()};
        if (*bit)
            return std::unexpected{transaction_error("SBNK already links this Program")};
    }
    auto payload = detail::prepare_prog_payload(spec);
    if (!payload)
        return std::unexpected{payload.error()};
    auto allocated = allocate_record(partition, std::move(*payload), PayloadKind::object);
    if (!allocated)
        return std::unexpected{allocated.error()};
    if (auto appended = append_directory_entry(state, partition, *directory, allocated->first, name, cancellation);
        !appended)
        return std::unexpected{appended.error()};
    for (const auto &target : targets) {
        if (target.assignment->target_kind == "SBNK") {
            if (auto updated = set_sbnk_program_bit(state, partition, target.id, spec.number, true, cancellation);
                !updated)
                return std::unexpected{updated.error()};
        }
        state.known_edges.emplace_back(*partition_index, allocated->first, target.id);
    }
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
    if (sample_bank == nullptr || sample_bank->stored_member_count > sample_bank->maximum_member_count) {
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
    for (const auto &slot : sample_bank->slots) {
        if (slot.active)
            members.insert(slot.name);
    }
    for (const auto &other : *sample_banks) {
        if (other.id == located->second)
            continue;
        const auto *other_sample_bank = std::get_if<CurrentSbac>(&other.decoded.payload);
        for (const auto &slot : other_sample_bank->slots) {
            if (slot.active && members.contains(slot.name)) {
                return std::unexpected{transaction_error("another Sample Bank shares a Sample")};
            }
        }
    }
    for (const auto &slot : sample_bank->slots) {
        if (!slot.active)
            continue;
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
    for (const auto &existing : *existing_sample_banks) {
        if (ascii_case_equal(existing.name, spec.name)) {
            return std::unexpected{transaction_error("Sample Bank already exists")};
        }
    }
    auto memberships = sample_bank_memberships(*existing_sample_banks);
    if (!memberships)
        return std::unexpected{memberships.error()};
    std::map<std::string, SampleSpec> sample_specs;
    std::map<std::string, SfsId> member_ids;
    for (const auto &name : spec.member_samples) {
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
        if (current_sample == nullptr)
            return std::unexpected{transaction_error("Sample is unreadable")};
        if (!current_sample->linked_program_numbers.empty())
            return std::unexpected{transaction_error("Sample is assigned directly to a Program")};
        const auto banked = (std::to_integer<std::uint8_t>((*sample_payload)[0xd0U]) & 1U) != 0U;
        const auto sources = memberships->find(name);
        const auto source_count = sources == memberships->end() ? 0U : sources->second.size();
        if (source_count > 1U)
            return std::unexpected{transaction_error("Sample is shared by multiple Sample Banks")};
        if (banked != (source_count == 1U))
            return std::unexpected{transaction_error("Sample membership flag disagrees with its Sample Bank")};
        SampleSpec placeholder;
        placeholder.name = name;
        sample_specs.emplace(name, std::move(placeholder));
        member_ids.emplace(name, sample->second);
    }
    if (auto detached = detach_sample_bank_members(state, partition, *existing_sample_banks, member_ids,
                                                   *partition_index, cancellation);
        !detached) {
        return std::unexpected{detached.error()};
    }
    auto payload = detail::prepare_sbac_payload(spec, sample_specs);
    if (!payload)
        return std::unexpected{payload.error()};
    auto allocated = allocate_record(partition, std::move(*payload), PayloadKind::object);
    if (!allocated)
        return std::unexpected{allocated.error()};
    for (const auto &[name, id] : member_ids) {
        static_cast<void>(name);
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
        sample_bank->slots.size() != sample_bank->stored_member_count) {
        return std::unexpected{transaction_error("SBAC rename requires a nonempty fully readable slot table")};
    }
    std::set<SfsId> member_ids;
    for (const auto &slot : sample_bank->slots) {
        if (!slot.active)
            continue;
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
                if (!slot.active)
                    return false;
                return std::ranges::any_of(sample_bank->slots,
                                           [&](const SbacSlot &own) { return own.active && own.name == slot.name; });
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
