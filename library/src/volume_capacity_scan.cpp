#include "axklib/volume_capacity.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "axklib/bytes.hpp"
#include "axklib/object.hpp"
#include "axklib/volume_capacity_internal.hpp"

namespace axk {
namespace {

std::array<std::byte, 16> name_at(std::span<const std::byte> bytes, std::size_t offset) {
    std::array<std::byte, 16> result{};
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), result.size(), result.begin());
    return result;
}

bool category_name(std::string_view name) {
    return name == "SBNK" || name == "SBAC" || name == "PROG" || name == "SMPL" || name == "SEQU";
}

bool canonical_path_name(std::string_view name) {
    return !name.empty() && std::ranges::all_of(name, [](unsigned char value) {
        return value >= 0x20U && value <= 0x7eU && value != 0x5cU;
    });
}

Result<std::vector<DirectoryEntry>> read_directory_stream(const Container &container, const Partition &partition,
                                                          const IndexRecord &directory,
                                                          const CancellationToken &cancellation) {
    const auto malformed = [] {
        return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                          "A malformed directory stream prevents complete capacity inspection")};
    };
    if (directory.data_size % 32U != 0U)
        return malformed();
    std::vector<DirectoryEntry> entries;
    bool ended{};
    for (std::uint64_t begin = 0U; begin < directory.data_size;) {
        const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(65536U, directory.data_size - begin));
        const auto bytes = container.read_record_range(partition.index, directory.sfs_id, begin, size, cancellation);
        if (!bytes)
            return std::unexpected{bytes.error()};
        const ByteReader reader{*bytes};
        for (std::size_t offset = 0U; offset < bytes->size(); offset += 32U) {
            const auto row = std::span<const std::byte>{*bytes}.subspan(offset, 32U);
            const auto raw_link = LinkId{*reader.be32(offset + 4U)};
            if (ended || (*reader.be32(offset) == 0U && raw_link.value == 0U)) {
                if (!std::ranges::all_of(row, [](std::byte value) { return value == std::byte{}; }))
                    return malformed();
                ended = true;
                continue;
            }
            const auto length = *reader.be16(offset + 2U);
            if (*reader.be16(offset) != 32U || length == 0U || length > 24U || row[8U + length - 1U] != std::byte{})
                return malformed();
            const auto name = reader.ascii_field(offset + 8U, length, false);
            if (!name)
                return std::unexpected{name.error()};
            const auto state = directory_entry_state(raw_link);
            entries.push_back({32U, raw_link,
                               state == DirectoryEntryState::live ? std::optional{raw_link} : std::nullopt, state,
                               *name, begin + offset});
        }
        begin += size;
    }
    return entries;
}

Result<std::string> path_name(const Container &container, const Partition &partition, const IndexRecord &directory,
                              const DirectoryEntry &entry, const CancellationToken &cancellation) {
    const auto row = container.read_record_range(partition.index, directory.sfs_id, entry.payload_relative_offset, 32U,
                                                 cancellation);
    if (!row)
        return std::unexpected{row.error()};
    const auto length = ByteReader{*row}.be16(2U);
    if (!length || *length < 2U || *length > 24U)
        return std::string{};
    const auto bytes = std::span<const std::byte>{*row}.subspan(8U, *length - 1U);
    std::string result;
    for (const auto byte : bytes)
        result.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    if (!canonical_path_name(result) || (*row)[8U + *length - 1U] != std::byte{})
        return std::string{};
    return result;
}

Result<detail::CapacityObject> read_metadata(const Container &container, const Partition &partition,
                                             const IndexRecord &record, const CancellationToken &cancellation) {
    const auto prefix = container.read_record_range(partition.index, record.sfs_id, 0U, 0x42U, cancellation);
    if (!prefix)
        return std::unexpected{prefix.error()};
    const auto header = decode_object_header(*prefix);
    if (!header)
        return std::unexpected{header.error()};
    detail::CapacityObject result;
    result.type = header->type;
    result.name = name_at(*prefix, 0x32U);
    result.selector = header->unknown_0x14;
    result.older_body_bytes = header->record_size_or_header_used;
    result.encoded_body_bytes = result.selector < 4U ? header->record_size_or_header_used : header->payload_bytes_0x1c;
    result.physical_body_bytes = record.data_size >= 0x30U ? record.data_size - 0x30U : 0U;
    if (result.type == ObjectType::sequ) {
        return result;
    }
    const std::size_t size = result.type == ObjectType::sbnk   ? 0xd1U
                             : result.type == ObjectType::sbac ? 0x14cU
                             : result.type == ObjectType::prog ? 0x120U
                                                               : 0xacU;
    const auto bytes = container.read_record_range(partition.index, record.sfs_id, 0U, size, cancellation);
    if (!bytes)
        return std::unexpected{bytes.error()};
    const ByteReader reader{*bytes};
    if (result.type == ObjectType::sbnk) {
        if (bytes->size() < 0xd1U)
            return std::unexpected{
                make_error(ErrorCode::object_malformed, ErrorCategory::object, "Sample metadata is truncated")};
        result.has_left_wave = (*bytes)[0x78U] != std::byte{};
        if (result.has_left_wave)
            result.references.push_back({ObjectType::smpl, name_at(*bytes, 0x78U)});
        if ((*bytes)[0x88U] != std::byte{})
            result.references.push_back({ObjectType::smpl, name_at(*bytes, 0x88U)});
        result.bank_member = ((*bytes)[0xd0U] & std::byte{1U}) != std::byte{};
        return result;
    }
    if (result.type == ObjectType::sbac || result.type == ObjectType::prog) {
        const bool bank = result.type == ObjectType::sbac;
        const std::size_t start = bank ? 0x14cU : 0x120U;
        const std::size_t stride = bank ? 20U : 56U;
        const std::size_t tail = result.selector < 4U ? 0U : (bank ? 36U : 176U);
        const auto logical_end = std::uint64_t{0x30U} + result.encoded_body_bytes;
        if (logical_end < start + tail || logical_end > record.data_size || (logical_end - start - tail) % stride != 0U)
            return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                              "Counted assignment metadata has an invalid body extent")};
        result.physical_rows = static_cast<std::size_t>((logical_end - start - tail) / stride);
        result.counted_rows = bank ? *reader.u8(0x144U) : *reader.be16(0x96U);
        if (result.counted_rows > result.physical_rows)
            return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                              "Counted assignment rows exceed the stored body")};
        const auto rows = container.read_record_range(partition.index, record.sfs_id, start,
                                                      result.counted_rows * stride, cancellation);
        if (!rows)
            return std::unexpected{rows.error()};
        const ByteReader row_reader{*rows};
        for (std::size_t index = 0U; index < result.counted_rows; ++index) {
            const auto offset = index * stride;
            const auto kind = bank                                        ? ObjectType::sbnk
                              : (*rows)[offset + 20U] == std::byte{0x10U} ? ObjectType::sbnk
                              : (*rows)[offset + 20U] == std::byte{0x11U} ? ObjectType::sbac
                                                                          : ObjectType::unknown;
            const auto name = name_at(*rows, offset);
            const auto handle = *row_reader.be32(offset + 16U);
            result.rows.push_back({kind, name, handle});
            if (name.front() != std::byte{}) {
                result.references.push_back({kind, name, handle});
                result.canonical_row_handles &= handle == 0U;
            } else {
                ++result.empty_counted_rows;
                if (handle != 0U)
                    result.inactive_row_handles.push_back(handle);
            }
        }
        return result;
    }
    const auto decoded = decode_object(*bytes);
    if (!decoded)
        return std::unexpected{decoded.error()};
    if (const auto *wave = std::get_if<CurrentSmpl>(&decoded->payload)) {
        if (wave->stored_pcm_offset < 0xacU || wave->stored_pcm_offset > record.data_size ||
            wave->stored_segment_bytes > record.data_size - wave->stored_pcm_offset ||
            wave->stored_segment_offset != 0U || wave->stored_segment_bytes != wave->stored_pcm_bytes)
            return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                              "Capacity inspection requires complete, physically bounded Wave Data")};
    }
    return result;
}

} // namespace

Result<VolumeCapacityReport> inspect_volume_capacity(const Container &container, PartitionIndex partition_index,
                                                     SfsId volume_directory, const CancellationToken &cancellation) {
    const auto checked = cancellation.check();
    if (!checked)
        return std::unexpected{checked.error()};
    const auto partition = std::ranges::find(container.partitions(), partition_index, &Partition::index);
    if (partition == container.partitions().end())
        return std::unexpected{make_error(ErrorCode::container_partition_out_of_range, ErrorCategory::container,
                                          "Capacity inspection partition does not exist")};
    const auto root_id = locate_partition_root_record(*partition);
    if (!root_id)
        return std::unexpected{root_id.error()};
    const auto root = std::ranges::find(partition->records, *root_id, &IndexRecord::sfs_id);
    const auto volume = std::ranges::find(partition->records, volume_directory, &IndexRecord::sfs_id);
    if (root == partition->records.end() || volume == partition->records.end() || volume == root ||
        !volume->directory_id || !root->directory_id || volume->parent_directory_id != root->directory_id)
        return std::unexpected{make_error(ErrorCode::object_missing, ErrorCategory::object,
                                          "Capacity inspection requires a physical volume directory")};
    detail::CapacityVolume input{partition_index, volume_directory, {}, {}, {}};
    const auto root_entries = read_directory_stream(container, *partition, *root, cancellation);
    if (!root_entries)
        return std::unexpected{root_entries.error()};
    const auto volume_entries = read_directory_stream(container, *partition, *volume, cancellation);
    if (!volume_entries)
        return std::unexpected{volume_entries.error()};
    std::size_t root_links{};
    for (const auto &entry : *root_entries) {
        if (entry.state == DirectoryEntryState::live && entry.target_link_id == volume->directory_id &&
            entry.name != "." && entry.name != ".." && !is_partition_support_root_entry(entry.name)) {
            input.name = entry.name;
            ++root_links;
            const auto raw = path_name(container, *partition, *root, entry, cancellation);
            if (!raw)
                return std::unexpected{raw.error()};
            if (raw->empty() || raw->size() > 16U)
                input.issues.push_back(
                    {"UNSUPPORTED_DIRECTORY_NAME", "A directory name has an unsupported raw encoding."});
        }
    }
    if (root_links != 1U)
        input.issues.push_back({"AMBIGUOUS_VOLUME", "The physical volume has no unique root-directory placement."});
    const auto same_name = std::ranges::count_if(*root_entries, [&](const DirectoryEntry &entry) {
        return entry.state == DirectoryEntryState::live && entry.name == input.name;
    });
    if (same_name != 1 || !canonical_path_name(input.name))
        input.issues.push_back(
            {"AMBIGUOUS_VOLUME_NAME", "The volume does not have a unique loadable filesystem name."});
    std::map<std::uint32_t, std::vector<const IndexRecord *>> by_link;
    std::map<std::uint32_t, const IndexRecord *> by_record;
    for (const auto &record : partition->records) {
        if (record.directory_id)
            by_link[record.directory_id->value].push_back(&record);
        by_record.emplace(record.sfs_id.value, &record);
    }
    std::set<std::string> category_names;
    for (const auto &category_entry : *volume_entries) {
        if (category_entry.state != DirectoryEntryState::live || category_entry.name == "." ||
            category_entry.name == "..")
            continue;
        if (!category_name(category_entry.name))
            continue;
        if (!category_names.insert(category_entry.name).second)
            input.issues.push_back(
                {"DUPLICATE_CATEGORY_NAME", "Multiple category directories have the same load path."});
        const auto category_path = path_name(container, *partition, *volume, category_entry, cancellation);
        if (!category_path)
            return std::unexpected{category_path.error()};
        if (*category_path != category_entry.name)
            input.issues.push_back({"UNSUPPORTED_DIRECTORY_NAME", "A category name has an unsupported raw encoding."});
        const auto category_it =
            category_entry.target_link_id ? by_link.find(category_entry.target_link_id->value) : by_link.end();
        if (category_it == by_link.end() || category_it->second.size() != 1U ||
            category_it->second.front()->parent_directory_id != volume->directory_id) {
            input.issues.push_back(
                {"AMBIGUOUS_CATEGORY", "A category directory has a missing or ambiguous physical placement."});
            continue;
        }
        const auto &category = *category_it->second.front();
        const auto category_entries = read_directory_stream(container, *partition, category, cancellation);
        if (!category_entries)
            return std::unexpected{category_entries.error()};
        std::set<std::string> filenames;
        for (const auto &entry : *category_entries) {
            if (entry.state != DirectoryEntryState::live || entry.name == "." || entry.name == "..")
                continue;
            if (!filenames.insert(entry.name).second || !canonical_path_name(entry.name))
                input.issues.push_back(
                    {"AMBIGUOUS_FILENAME", "An object does not have a unique loadable filesystem name."});
            const auto found = entry.target_link_id ? by_record.find(entry.target_link_id->value) : by_record.end();
            if (found == by_record.end() || found->second->payload_kind != PayloadKind::object) {
                input.issues.push_back(
                    {"AMBIGUOUS_OBJECT_PLACEMENT", "An object has a missing physical directory placement."});
                continue;
            }
            auto metadata = read_metadata(container, *partition, *found->second, cancellation);
            if (!metadata)
                return std::unexpected{metadata.error()};
            const auto raw = path_name(container, *partition, category, entry, cancellation);
            if (!raw)
                return std::unexpected{raw.error()};
            if (raw->empty() || raw->size() > 16U)
                input.issues.push_back({"UNSUPPORTED_FILENAME_ENCODING",
                                        category_entry.name + "/" + entry.name +
                                            ": the filesystem name exceeds the sampler's 16-byte object-name limit."});
            else {
                std::array<std::byte, 16> padded;
                padded.fill(std::byte{' '});
                std::ranges::transform(*raw, padded.begin(), [](char value) { return static_cast<std::byte>(value); });
                metadata->filesystem_name = padded;
            }
            if (detail::capacity_type_name(metadata->type) != category_entry.name)
                input.issues.push_back(
                    {"CATEGORY_TYPE_MISMATCH", "An object's type differs from its category directory."});
            input.objects.push_back(*metadata);
        }
    }
    return detail::analyze_volume_capacity(input);
}

} // namespace axk
