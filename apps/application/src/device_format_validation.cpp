#include "device_format_internal.hpp"
#include "device_sfs_validation.hpp"

#include <algorithm>

#include "axklib/catalog.hpp"

namespace axk::app::format_detail {
Result<void> require_safe(const DeviceWriteSafety &safety) {
    if (!safety.complete || !safety.non_dismounting_claim_supported || safety.membership.empty() ||
        safety.recovery_membership.empty() || safety.identity.device_path.empty() || !safety.mounted.empty())
        return std::unexpected(Error{"device_format_blocked", "Native carrier safety does not permit formatting"});
    return {};
}

Result<void> require_fresh(const Connection &connection) {
    if (connection.invalidated || connection.mutation_active || !connection.initialization_operation.empty() ||
        !connection.authorized_target.empty() || !connection.metadata_guards.empty() ||
        connection.device.transport->has_write_handle())
        return std::unexpected(
            Error{"device_busy", "Formatting requires a fresh read-only connection; eject and select again"});
    return verify_bound(connection);
}

Result<Guards> read_initialization_guards(const Connection &connection, const DeviceTargetDescription &target,
                                          const HdsInitialization &initialization, Guards guards,
                                          const CancellationToken &cancellation) {
    for (const auto &write : initialization.writes) {
        for (std::size_t at = 0U; at < write.bytes.size();) {
            if (cancellation.is_cancelled())
                return std::unexpected(Error{"operation_cancelled", "Format review cancelled"});
            const auto count = std::min<std::size_t>(64U * 1024U, write.bytes.size() - at);
            std::vector<std::byte> bytes(count);
            const auto offset = target.offset + write.offset + at;
            if (auto read = connection.device.transport->read_exact_at(offset, bytes); !read)
                return std::unexpected(Error{"device_read_failed", read.error().message});
            for (std::size_t index = 0U; index < count; index += 512U) {
                const auto hash = *digest(std::span{bytes}.subspan(index, 512U));
                const auto found = guards.find(offset + index);
                if (found != guards.end() && found->second != hash)
                    return std::unexpected(Error{"device_changed", "Device contents changed during format review"});
                if (guards.size() >= maximum_guard_sectors && found == guards.end())
                    return std::unexpected(Error{"device_guard_capacity", "Format review guard budget is exhausted"});
                guards[offset + index] = hash;
            }
            at += count;
        }
    }
    return guards;
}

Result<void> check_initialized(std::shared_ptr<const RandomAccessReader> reader,
                               const HdsInitialization &initialization) {
    for (const auto &write : initialization.writes) {
        for (std::size_t at = 0U; at < write.bytes.size();) {
            const auto count = std::min<std::size_t>(64U * 1024U, write.bytes.size() - at);
            std::vector<std::byte> bytes(count);
            if (auto read = reader->read_exact_at(write.offset + at, bytes); !read)
                return std::unexpected(Error{"device_format_readback", read.error().message});
            if (!std::equal(bytes.begin(), bytes.end(), write.bytes.begin() + static_cast<std::ptrdiff_t>(at)))
                return std::unexpected(Error{"device_format_readback",
                                             "Initialized filesystem metadata differs from the reviewed layout"});
            at += count;
        }
    }
    auto image = open_image(reader, {});
    if (!image || !valid_sfs_envelope(*image, initialization.size_bytes) ||
        image->partitions().size() != initialization.partitions.size())
        return std::unexpected(Error{"device_format_validation", "Initialized SFS failed strict envelope validation"});
    for (std::size_t index = 0U; index < initialization.partitions.size(); ++index) {
        const auto &expected = initialization.partitions[index];
        const auto &partition = image->partitions()[index];
        if (partition.start_sector != expected.start_sector ||
            partition.sector_count != expected.filesystem_sector_count ||
            partition.cluster_count != expected.cluster_count ||
            partition.directory_index_cluster != expected.directory_index_cluster)
            return std::unexpected(
                Error{"device_format_validation", "Initialized partition geometry differs from the plan"});
        const auto root = locate_partition_root_record(partition);
        if (!root)
            return std::unexpected(Error{"device_format_validation", root.error().message});
        const auto found = std::ranges::find(partition.records, *root, &IndexRecord::sfs_id);
        if (found == partition.records.end() || std::ranges::any_of(found->directory_entries, [](const auto &entry) {
                return entry.state == DirectoryEntryState::live && entry.name != "." && entry.name != ".." &&
                       !is_partition_support_root_entry(entry.name);
            }))
            return std::unexpected(
                Error{"device_format_validation", "Initialized SFS unexpectedly contains user volumes"});
    }
    auto catalog = build_object_catalog(*image);
    if (!catalog || !catalog->objects.empty())
        return std::unexpected(Error{"device_format_validation", "Initialized SFS unexpectedly contains user objects"});
    return {};
}
} // namespace axk::app::format_detail
