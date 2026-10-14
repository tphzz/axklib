#include "image_sessions_internal.hpp"
#include "session_state.hpp"

#include "axklib/sfs_repair.hpp"

std::vector<std::string> axk::app::formats::a_series::SessionState::available_operations(bool source_writable,
                                                                                         bool device_writable) const {
    const auto format = media_kind_name(media->kind());
    std::vector<std::string> available_operations{
        "images.content",           "images.objects",         "images.relationships", "images.systemProgramContexts",
        "images.validation.issues", "images.preview",         "auditions.prepare",    "images.package.export",
        "images.audio_export",      "images.sequence_export",
    };
    if (format == "sfs" || format == "iso9660")
        available_operations.emplace_back("images.volume_package_export");
    if (format == "sfs")
        available_operations.emplace_back("images.volume_capacity.inspect");
    if (format == "sfs" && source_reference.kind == ImageSourceKind::file) {
        available_operations.emplace_back("images.volume_floppy_export");
        available_operations.emplace_back("images.media_conversion");
    }
    const auto device_source = source_reference.kind == ImageSourceKind::device;
    const auto *mutable_container = media ? std::get_if<Container>(&media->storage()) : nullptr;
    if (format == "sfs" && source_reference.kind == ImageSourceKind::file && mutable_container != nullptr &&
        inspect_sfs_extent_layout_repair(*mutable_container)) {
        available_operations.emplace_back("images.extent_layout.repair");
    }
    const auto supported_mutation_profile =
        mutable_container != nullptr && mutable_container->superblock().sector_size_bytes == 512U &&
        std::ranges::all_of(mutable_container->partitions(), [](const Partition &partition) {
            return partition.sectors_per_cluster == 2U && allocation_is_safe_for_mutation(partition.allocation) &&
                   locate_partition_root_record(partition).has_value();
        });
    if (format == "sfs" && supported_mutation_profile && (device_writable || source_writable)) {
        available_operations.emplace_back("images.alter.volumes");
        if (!device_source)
            available_operations.emplace_back("images.alter.partitions");
        available_operations.emplace_back("images.alter.objects");
        available_operations.emplace_back("images.package.import");
        available_operations.emplace_back("images.floppy.import");
        available_operations.emplace_back("images.deletion.orphans.inspect");
        available_operations.emplace_back("images.programs.generate.inspect");
        available_operations.emplace_back("images.programs.generate");
        available_operations.emplace_back("images.program_assignments.cleanup.inspect");
        available_operations.emplace_back("images.program_assignments.cleanup");
    }
    return available_operations;
}
