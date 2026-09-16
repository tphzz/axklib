#include "image_filesystem_attributes.hpp"

#include <format>
#include <limits>

namespace axk::app::detail {
void describe_sfs_attributes(ImageFilesystemEntry &entry, const IndexRecord &record, const Partition &partition,
                             std::uint32_t sector_bytes) {
    entry.raw_attributes = std::format("SFS 0x{:08X}", record.attributes);
    entry.attributes.clear();
    const auto type = record.attributes & 0x01ffffffU;
    const bool directory = type == 0x00646972U;
    if (type == 0U || directory) {
        const std::string state = (record.attributes & 0x08000000U) != 0U ? "Enabled" : "Disabled";
        entry.attributes.push_back({directory ? "sfs.directory-write" : "sfs.file-write",
                                    directory ? "Temporary directory write flag" : "File write flag", state,
                                    directory
                                        ? "Enabled: directory data writes are temporarily enabled.\n"
                                          "Disabled: the temporary write flag is clear.\n\n"
                                          "Directory updates enable this flag as needed and clear it afterward."
                                        : "Enabled: permits file data writes and extension.\n"
                                          "Disabled: blocks file data writes and extension.\n\n"
                                          "Writing also requires a writable handle and partition. Rename, deletion "
                                          "and overall image editability are governed by separate checks.",
                                    directory ? "" : "File write flag: " + state});
    }
    entry.attributes.push_back({"sfs.record-state", "Record state",
                                (record.attributes & 0x80000000U) != 0U ? "Live" : "Inactive",
                                "Live: the record is marked active.\nInactive: the live marker is clear.\n\n"
                                "The live marker is set when the record is created and cleared on final release.",
                                ""});
    if (type == 0U || directory)
        entry.attributes.push_back({"sfs.record-type", "Native record type", directory ? "Directory" : "Ordinary",
                                    "Ordinary: holds file data.\nDirectory: contains filesystem entries.\n\n"
                                    "Identifies the record's role in the SFS filesystem.",
                                    ""});
    const bool large = (record.attributes & 0x20000000U) != 0U;
    entry.attributes.push_back(
        {"sfs.allocation", "Allocation policy", large ? "Large unit" : "Standard",
         "Standard: ordinary cluster allocation; ordinary growth uses at least two clusters.\n"
         "Large unit: payload extents are aligned and rounded to the partition's larger allocation "
         "unit.\n\nAllocated capacity can exceed the logical file size under either policy.",
         ""});
    if (large && partition.large_allocation_unit_clusters != 0U) {
        const auto cluster_bytes = static_cast<std::uint64_t>(partition.sectors_per_cluster) * sector_bytes;
        const auto unit = partition.large_allocation_unit_clusters;
        const auto value = cluster_bytes != 0U && unit <= std::numeric_limits<std::uint64_t>::max() / cluster_bytes
                               ? std::format("{} clusters ({} B)", unit, unit * cluster_bytes)
                               : std::format("{} clusters", unit);
        entry.attributes.push_back({"sfs.allocation-unit", "Allocation unit", value,
                                    "Size of the partition's large payload allocation unit, in clusters and equivalent "
                                    "bytes.\n\nA cluster is a group of disk sectors. The allocation unit specifies "
                                    "the increment used when allocating payload storage.",
                                    ""});
    }
    entry.attributes.push_back({"sfs.references", "Filesystem references", std::to_string(record.link_count),
                                "Number of filesystem references to this record.\n\nOrdinary files normally have one "
                                "reference. A directory normally has two (its parent's entry and its own '.'), plus "
                                "one for each child directory's '..'. The root's '.' and '..' refer to itself.",
                                ""});
}

void describe_fat_attributes(ImageFilesystemEntry &entry, std::uint8_t bits) {
    entry.raw_attributes = std::format("FAT 0x{:02X}", bits);
    entry.attributes.clear();
    if ((bits & 0x01U) != 0U)
        entry.attributes.push_back({"fat.read-only", "Read-only", "Yes",
                                    "Yes: the FAT read-only flag is set, requesting protection from modification.\n\n"
                                    "When clear, this row is omitted.",
                                    "Read-only"});
    if ((bits & 0x02U) != 0U)
        entry.attributes.push_back({"fat.hidden", "Hidden", "Yes",
                                    "Yes: the FAT hidden flag is set.\n\n"
                                    "When clear, this row is omitted. File browsers may omit hidden entries from "
                                    "ordinary listings; Files shows them.",
                                    "Hidden"});
    if ((bits & 0x04U) != 0U)
        entry.attributes.push_back({"fat.system", "System", "Yes",
                                    "Yes: the FAT system flag is set, marking the entry as a system file.\n\n"
                                    "When clear, this row is omitted.",
                                    "System"});
    if ((bits & 0x20U) != 0U)
        entry.attributes.push_back({"fat.archive", "Archive", "Yes",
                                    "Yes: the FAT archive flag is set, conventionally marking a file as changed since "
                                    "backup.\n\nWhen clear, this row is omitted.",
                                    "Archive"});
}
} // namespace axk::app::detail
