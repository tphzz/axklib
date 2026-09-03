#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string_view>

#include <nlohmann/json.hpp>

#include "axklib/sfs.hpp"

namespace {

std::string_view payload_kind(axk::PayloadKind kind) {
    switch (kind) {
    case axk::PayloadKind::directory:
        return "directory";
    case axk::PayloadKind::object:
        return "object";
    case axk::PayloadKind::unknown:
        return "unknown";
    }
    return "unknown";
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    const auto opened = axk::open_image(std::filesystem::path{argv[1]});
    if (!opened) {
        std::cerr << axk::render_error(opened.error()) << '\n';
        return 1;
    }
    nlohmann::ordered_json partitions = nlohmann::ordered_json::array();
    for (const auto &partition : opened->partitions()) {
        nlohmann::ordered_json records = nlohmann::ordered_json::array();
        for (const auto &record : partition.records) {
            nlohmann::ordered_json entries = nlohmann::ordered_json::array();
            for (const auto &entry : record.directory_entries) {
                entries.push_back({{"flags", entry.flags},
                                   {"raw_link_id", entry.raw_link_id.value},
                                   {"target_link_id", entry.target_link_id ? nlohmann::json{entry.target_link_id->value}
                                                                           : nlohmann::json{}},
                                   {"state", entry.state == axk::DirectoryEntryState::live ? "live" : "deleted"},
                                   {"name", entry.name}});
            }
            records.push_back({{"sfs_id", record.sfs_id.value},
                               {"extent_count", record.extent_count},
                               {"cluster_count", record.cluster_count},
                               {"data_size", record.data_size},
                               {"payload_kind", payload_kind(record.payload_kind)},
                               {"object_type", record.object_type},
                               {"object_name", record.object_name},
                               {"directory_id", record.directory_id ? nlohmann::ordered_json(record.directory_id->value)
                                                                    : nlohmann::ordered_json(nullptr)},
                               {"parent_directory_id", record.parent_directory_id
                                                           ? nlohmann::ordered_json(record.parent_directory_id->value)
                                                           : nlohmann::ordered_json(nullptr)},
                               {"directory_entries", std::move(entries)}});
        }
        nlohmann::ordered_json free_space = nullptr;
        if (partition.allocation.free_space) {
            const auto &free = *partition.allocation.free_space;
            free_space = {{"reserved_cluster_count", free.reserved_cluster_count},
                          {"allocated_cluster_count", free.allocated_cluster_count},
                          {"free_cluster_count", free.free_cluster_count},
                          {"free_bytes", free.free_bytes},
                          {"sampler_visible_free_kib", free.sampler_visible_free_kib}};
        }
        partitions.push_back(
            {{"index", partition.index.value},
             {"name", partition.name},
             {"start_sector", partition.start_sector},
             {"sector_count", partition.sector_count},
             {"cluster_count", partition.cluster_count},
             {"sectors_per_cluster", partition.sectors_per_cluster},
             {"bitmap_cluster", partition.bitmap_cluster},
             {"directory_index_cluster", partition.directory_index_cluster},
             {"directory_index_span_clusters", partition.directory_index_span_clusters},
             {"backup_header_matches", partition.backup_header_matches},
             {"records", std::move(records)},
             {"allocation",
              {{"fixed_bitmap_used_cluster_count", partition.allocation.fixed_location.used_cluster_count},
               {"header_bitmap_used_cluster_count", partition.allocation.header_addressed.used_cluster_count},
               {"bitmap_copies_match", partition.allocation.stored_copies_match},
               {"reconstructed_used_cluster_count", partition.allocation.reconstructed_used_cluster_count},
               {"invalid_extent_record_count", partition.allocation.invalid_extent_record_count},
               {"extent_total_mismatch_count", partition.allocation.extent_total_mismatch_count},
               {"extent_byte_total_mismatch_count", partition.allocation.extent_byte_total_mismatch_count},
               {"free_space", std::move(free_space)}}}});
    }
    const nlohmann::ordered_json result = {
        {"schema_version", "1.0"},
        {"container", "yamaha_sfs"},
        {"image_size_bytes", opened->image_size_bytes()},
        {"sector_size_bytes", opened->superblock().sector_size_bytes},
        {"total_sector_count", opened->superblock().total_sector_count},
        {"backup_superblock_matches", opened->backup_superblock_matches()},
        {"partitions", std::move(partitions)},
    };
    std::cout << result.dump() << '\n';
    return 0;
}
