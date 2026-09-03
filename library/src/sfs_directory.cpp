#include "sfs_internal.hpp"

#include "axklib/bytes.hpp"
#include "axklib/object.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace axk::sfs_detail {

DirectoryEntry make_directory_entry(std::uint16_t flags, std::uint32_t raw_link, std::string name,
                                    std::uint64_t offset) {
    const auto raw_link_id = LinkId{raw_link};
    const auto state = directory_entry_state(raw_link_id);
    return {
        flags, raw_link_id,     state == DirectoryEntryState::live ? std::optional<LinkId>{raw_link_id} : std::nullopt,
        state, std::move(name), offset,
    };
}

std::vector<DirectoryEntry> parse_directory_entries(std::span<const std::byte> payload) {
    std::vector<DirectoryEntry> result;
    const ByteReader reader{payload};
    for (std::size_t offset = 0; offset + 32U <= payload.size(); offset += 32U) {
        const auto prefix = reader.be32(offset);
        const auto link = reader.be32(offset + 4U);
        if (!prefix || !link || (*prefix == 0 && *link == 0))
            break;
        const auto flags = reader.be16(offset);
        const auto name_size = reader.be16(offset + 2U);
        if (!flags || !name_size || *name_size == 0 || *name_size > 24U)
            break;
        const auto name = reader.ascii_field(offset + 8U, *name_size, false);
        if (!name)
            break;
        result.push_back(make_directory_entry(*flags, *link, *name, offset));
    }
    return result;
}

void classify_record(IndexRecord &record, std::span<const std::byte> payload) {
    if (const auto envelope = decode_current_record_envelope(payload); envelope) {
        record.payload_kind = PayloadKind::object;
        record.object_type = envelope->raw_type;
        if (const auto header = decode_object_header(payload); header)
            record.object_name = header->name;
        return;
    }
    auto entries = parse_directory_entries(payload);
    if (entries.size() >= 2U && entries[0].name == "." && entries[1].name == ".." && entries[0].target_link_id &&
        entries[1].target_link_id) {
        record.payload_kind = PayloadKind::directory;
        record.directory_id = entries[0].target_link_id;
        record.parent_directory_id = entries[1].target_link_id;
        record.directory_entries = std::move(entries);
    }
}

Result<void> validate_directory_graph(Partition &partition, const OpenOptions &options) {
    std::unordered_map<std::uint32_t, const IndexRecord *> records_by_id;
    std::unordered_map<std::uint32_t, const IndexRecord *> directories_by_id;
    for (const auto &record : partition.records) {
        records_by_id.emplace(record.sfs_id.value, &record);
        if (!record.directory_id)
            continue;
        const bool inserted = directories_by_id.emplace(record.directory_id->value, &record).second;
        if (!inserted) {
            ErrorContext context;
            context.partition_index = partition.index.value;
            context.raw_offset = record.record_offset.value;
            partition.diagnostics.push_back(make_error(ErrorCode::relationship_ambiguous, ErrorCategory::relationship,
                                                       "multiple SFS directory records claim the same directory ID",
                                                       std::move(context)));
        }
        if (record.data_size > options.max_directory_bytes) {
            ErrorContext context;
            context.partition_index = partition.index.value;
            context.raw_offset = record.record_offset.value;
            partition.diagnostics.push_back(make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                                       "directory payload exceeds the configured traversal bound",
                                                       std::move(context)));
        }
    }

    if (directories_by_id.size() > options.max_directory_records) {
        ErrorContext context;
        context.partition_index = partition.index.value;
        partition.diagnostics.push_back(make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                                   "directory record count exceeds the configured traversal bound",
                                                   std::move(context)));
        return {};
    }

    for (const auto &[directory_id, directory] : directories_by_id) {
        const bool is_partition_root =
            directory->parent_directory_id && directory->parent_directory_id->value == directory_id;
        for (const auto &entry : directory->directory_entries) {
            if (entry.name == "." || !entry.target_link_id ||
                (is_partition_root && is_partition_support_root_entry(entry.name)))
                continue;
            if (!records_by_id.contains(entry.target_link_id->value)) {
                ErrorContext context;
                context.partition_index = partition.index.value;
                context.object_type = "directory-entry";
                context.object_name = entry.name;
                context.raw_offset = directory->record_offset.value + entry.payload_relative_offset;
                partition.diagnostics.push_back(
                    make_error(ErrorCode::relationship_unresolved, ErrorCategory::relationship,
                               "directory entry references a missing SFS record", std::move(context)));
            }
        }
    }

    struct VisitFrame {
        std::uint32_t directory_id{};
        std::size_t next_entry{};
        std::size_t depth{};
    };
    std::unordered_map<std::uint32_t, std::uint8_t> colors;
    std::vector<VisitFrame> stack;
    std::vector<std::uint32_t> traversal_order;
    std::unordered_set<std::uint32_t> scheduled;
    traversal_order.reserve(directories_by_id.size());
    scheduled.reserve(directories_by_id.size());
    for (const auto &[directory_id, directory] : directories_by_id) {
        if (!directory->parent_directory_id || directory->parent_directory_id->value == directory_id ||
            !directories_by_id.contains(directory->parent_directory_id->value)) {
            traversal_order.push_back(directory_id);
            scheduled.insert(directory_id);
        }
    }
    for (const auto &[directory_id, directory] : directories_by_id) {
        static_cast<void>(directory);
        if (scheduled.insert(directory_id).second)
            traversal_order.push_back(directory_id);
    }
    for (const auto directory_id : traversal_order) {
        if (colors[directory_id] != 0)
            continue;
        if (options.max_directory_depth == 0) {
            ErrorContext context;
            context.partition_index = partition.index.value;
            context.raw_offset = directories_by_id.at(directory_id)->record_offset.value;
            partition.diagnostics.push_back(make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                                       "directory depth exceeds the configured traversal bound",
                                                       std::move(context)));
            continue;
        }
        stack.push_back({directory_id, 0U, 1U});
        colors[directory_id] = 1;
        while (!stack.empty()) {
            if (const auto check = options.cancellation.check(); !check)
                return std::unexpected{check.error()};
            auto &frame = stack.back();
            const auto found = directories_by_id.find(frame.directory_id);
            if (found == directories_by_id.end() || frame.next_entry >= found->second->directory_entries.size()) {
                colors[frame.directory_id] = 2;
                stack.pop_back();
                continue;
            }
            const auto &entry = found->second->directory_entries[frame.next_entry++];
            if (entry.name == "." || entry.name == ".." || !entry.target_link_id ||
                !directories_by_id.contains(entry.target_link_id->value))
                continue;
            const auto child_depth = frame.depth + 1U;
            if (child_depth > options.max_directory_depth) {
                ErrorContext context;
                context.partition_index = partition.index.value;
                context.object_type = "directory-entry";
                context.object_name = entry.name;
                context.raw_offset = found->second->record_offset.value + entry.payload_relative_offset;
                partition.diagnostics.push_back(make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                                           "directory depth exceeds the configured traversal bound",
                                                           std::move(context)));
                continue;
            }
            const auto color = colors[entry.target_link_id->value];
            if (color == 1) {
                ErrorContext context;
                context.partition_index = partition.index.value;
                context.object_type = "directory-entry";
                context.object_name = entry.name;
                context.raw_offset = found->second->record_offset.value + entry.payload_relative_offset;
                partition.diagnostics.push_back(make_error(ErrorCode::relationship_cycle, ErrorCategory::relationship,
                                                           "directory child links contain a cycle",
                                                           std::move(context)));
            } else if (color == 0) {
                colors[entry.target_link_id->value] = 1;
                stack.push_back({entry.target_link_id->value, 0U, child_depth});
            }
        }
    }
    return {};
}

} // namespace axk::sfs_detail

namespace axk {

bool is_partition_support_root_entry(std::string_view name) noexcept {
    if (const auto terminator = name.find('\0'); terminator != std::string_view::npos)
        name = name.substr(0U, terminator);
    while (!name.empty() && name.back() == ' ')
        name.remove_suffix(1U);
    return name == "PRF3" || name == "sfserrlog" || name == "sfserram";
}

Result<SfsId> locate_partition_root_record(const Partition &partition) {
    std::vector<const IndexRecord *> roots;
    for (const auto &record : partition.records) {
        if (record.payload_kind == PayloadKind::directory && record.directory_id && record.parent_directory_id &&
            *record.directory_id == *record.parent_directory_id) {
            roots.push_back(&record);
        }
    }
    if (roots.size() == 1U)
        return roots.front()->sfs_id;

    ErrorContext context;
    context.partition_index = partition.index.value;
    return std::unexpected{
        make_error(roots.empty() ? ErrorCode::relationship_unresolved : ErrorCode::relationship_ambiguous,
                   ErrorCategory::relationship,
                   roots.empty() ? "partition has no root directory" : "partition has multiple root directories",
                   std::move(context))};
}

} // namespace axk
