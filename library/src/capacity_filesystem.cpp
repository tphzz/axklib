#include "axklib/filesystem_transaction.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "axklib/capacity_admission_internal.hpp"

namespace axk::detail {
namespace {
bool touches_volume(const FilesystemEdit &edit, std::string_view name) {
    return std::visit(
        [&](const auto &operation) {
            if (operation.path.empty() || operation.path.front() == name)
                return true;
            using Operation = std::decay_t<decltype(operation)>;
            if constexpr (std::is_same_v<Operation, RenameFilesystemEntry>)
                return operation.path.size() == 1U && operation.new_name == name;
            if constexpr (std::is_same_v<Operation, MoveFilesystemEntry>)
                return operation.destination_parent.empty() ? operation.path.back() == name
                                                            : operation.destination_parent.front() == name;
            return false;
        },
        edit);
}
} // namespace
Result<VolumeCapacityAdmission> inspect_filesystem_capacity(std::shared_ptr<const RandomAccessReader> source,
                                                            std::shared_ptr<const RandomAccessReader> preview,
                                                            PartitionIndex partition_index,
                                                            std::span<const FilesystemEdit> edits,
                                                            const VolumeCapacityPolicy &policy,
                                                            const CancellationToken &cancellation) {
    std::set<std::string> names;
    std::set<std::string> sampler_names;
    std::set<std::string> source_names;
    std::set<std::string> sequence_replacement_names;
    const auto sampler_category = [](std::string_view name) {
        return name == "PROG" || name == "SBAC" || name == "SBNK" || name == "SMPL" || name == "SEQU";
    };
    for (const auto &edit : edits) {
        if (const auto *rename = std::get_if<RenameFilesystemEntry>(&edit); rename && !rename->path.empty()) {
            if (rename->path.size() == 1U) {
                if (names.erase(rename->path.front()))
                    names.insert(rename->new_name);
                if (sampler_names.erase(rename->path.front()))
                    sampler_names.insert(rename->new_name);
                if (source_names.erase(rename->path.front()))
                    source_names.insert(rename->new_name);
            } else if (rename->path.size() == 2U &&
                       (sampler_category(rename->new_name) ||
                        (rename->path.back() != "SEQU" && sampler_category(rename->path.back())))) {
                names.insert(rename->path.front());
                sampler_names.insert(rename->path.front());
            }
        }
        if (const auto *remove = std::get_if<RemoveFilesystemEntry>(&edit); remove && remove->path.size() == 1U) {
            names.erase(remove->path.front());
            sampler_names.erase(remove->path.front());
            source_names.erase(remove->path.front());
        }
        if (const auto *put = std::get_if<PutFilesystemFile>(&edit); put && put->path.size() > 1U) {
            if (sampler_category(put->path[1])) {
                names.insert(put->path.front());
                sampler_names.insert(put->path.front());
            }
        }
        if (const auto *move = std::get_if<MoveFilesystemEntry>(&edit); move && !move->path.empty()) {
            if (move->path.size() >= 2U && sampler_category(move->path[1]))
                source_names.insert(move->path.front());
            if (move->path.size() == 1U && !move->destination_parent.empty()) {
                names.erase(move->path.front());
                sampler_names.erase(move->path.front());
            }
            const auto &destination_name =
                move->destination_parent.empty() ? move->path.back() : move->destination_parent.front();
            if (move->destination_parent.empty() ||
                (move->destination_parent.size() == 1U && sampler_category(move->path.back())))
                names.insert(destination_name);
            if (move->destination_parent.size() > 1U && sampler_category(move->destination_parent[1])) {
                names.insert(destination_name);
                sampler_names.insert(destination_name);
            }
        }
    }
    VolumeCapacityAdmission result;
    result.target = policy.target;
    if (names.empty() && source_names.empty())
        return result;
    OpenOptions options;
    options.cancellation = cancellation;
    if (std::ranges::any_of(edits, [](const FilesystemEdit &edit) {
            const auto *put = std::get_if<PutFilesystemFile>(&edit);
            return put && put->conflict == FileConflict::skip;
        })) {
        const auto original = open_image(source, {}, options);
        if (!original)
            return std::unexpected{original.error()};
        const auto old_partition = std::ranges::find(original->partitions(), partition_index, &Partition::index);
        if (old_partition != original->partitions().end()) {
            const auto old_root_id = locate_partition_root_record(*old_partition);
            if (!old_root_id)
                return std::unexpected{old_root_id.error()};
            std::erase_if(sampler_names, [&](const std::string &name) {
                const auto skipped = std::ranges::all_of(edits, [&](const FilesystemEdit &edit) {
                    if (!touches_volume(edit, name))
                        return true;
                    const auto *put = std::get_if<PutFilesystemFile>(&edit);
                    if (!put || put->conflict != FileConflict::skip)
                        return false;
                    const auto &path = put->path;
                    auto directory = std::ranges::find(old_partition->records, *old_root_id, &IndexRecord::sfs_id);
                    for (std::size_t i = 0; i < path.size(); ++i) {
                        if (directory == old_partition->records.end())
                            return false;
                        const auto entry =
                            std::ranges::find_if(directory->directory_entries, [&](const auto &candidate) {
                                return candidate.state == DirectoryEntryState::live && candidate.name == path[i];
                            });
                        if (entry == directory->directory_entries.end())
                            return false;
                        if (i + 1U == path.size())
                            return true;
                        directory = std::ranges::find(old_partition->records, entry->target_link_id,
                                                      &IndexRecord::directory_id);
                    }
                    return false;
                });
                if (skipped)
                    names.erase(name);
                return skipped;
            });
        }
    }
    const auto image = open_image(preview, {}, options);
    if (!image)
        return std::unexpected{image.error()};
    const auto partition = std::ranges::find(image->partitions(), partition_index, &Partition::index);
    if (partition == image->partitions().end())
        return std::unexpected{make_error(ErrorCode::container_partition_out_of_range, ErrorCategory::container,
                                          "Filesystem capacity partition is missing")};
    const auto root_id = locate_partition_root_record(*partition);
    if (!root_id)
        return std::unexpected{root_id.error()};
    const auto root = std::ranges::find(partition->records, *root_id, &IndexRecord::sfs_id);
    if (root == partition->records.end())
        return std::unexpected{
            make_error(ErrorCode::object_missing, ErrorCategory::object, "Filesystem root is missing")};
    std::vector<CapacityDestination> destinations;
    for (const auto &entry : root->directory_entries) {
        if (!names.contains(entry.name) || !entry.target_link_id || entry.state != DirectoryEntryState::live)
            continue;
        const auto volume = std::ranges::find(partition->records, entry.target_link_id, &IndexRecord::directory_id);
        if (volume == partition->records.end())
            continue;
        const auto has_objects = std::ranges::any_of(volume->directory_entries, [&](const DirectoryEntry &category) {
            return category.state == DirectoryEntryState::live && sampler_category(category.name);
        });
        if (has_objects)
            sampler_names.insert(entry.name);
    }
    for (const auto &name : sampler_names)
        destinations.emplace_back(partition_index, name);
    for (const auto &name : sampler_names) {
        if (std::ranges::all_of(edits, [&](const FilesystemEdit &edit) {
                if (!touches_volume(edit, name))
                    return true;
                const auto *put = std::get_if<PutFilesystemFile>(&edit);
                return put && put->path.size() == 3U && put->path[0] == name && put->path[1] == "SEQU";
            }))
            sequence_replacement_names.insert(name);
    }
    if (!source_names.empty() || !sequence_replacement_names.empty()) {
        const auto original = open_image(source, {}, options);
        if (!original)
            return std::unexpected{original.error()};
        const auto original_partition = std::ranges::find(original->partitions(), partition_index, &Partition::index);
        if (original_partition == original->partitions().end())
            return std::unexpected{
                make_error(ErrorCode::object_missing, ErrorCategory::object, "Original Files partition is missing")};
        const auto original_root_id = locate_partition_root_record(*original_partition);
        if (!original_root_id)
            return std::unexpected{original_root_id.error()};
        const auto original_root =
            std::ranges::find(original_partition->records, *original_root_id, &IndexRecord::sfs_id);
        if (original_root == original_partition->records.end())
            return std::unexpected{
                make_error(ErrorCode::object_missing, ErrorCategory::object, "Original Files root is missing")};
        for (const auto &entry : root->directory_entries) {
            if ((!source_names.contains(entry.name) && !sequence_replacement_names.contains(entry.name)) ||
                entry.state != DirectoryEntryState::live)
                continue;
            const auto volume = std::ranges::find(partition->records, entry.target_link_id, &IndexRecord::directory_id);
            if (volume == partition->records.end())
                continue;
            const auto previously_rooted =
                std::ranges::any_of(original_root->directory_entries, [&](const DirectoryEntry &old) {
                    const auto old_record =
                        std::ranges::find(original_partition->records, old.target_link_id, &IndexRecord::directory_id);
                    return old.state == DirectoryEntryState::live && old_record != original_partition->records.end() &&
                           old_record->sfs_id == volume->sfs_id;
                });
            if (!previously_rooted) {
                destinations.push_back({partition_index, entry.name, volume->sfs_id});
                continue;
            }
            const auto before = inspect_volume_capacity(*original, partition_index, volume->sfs_id, cancellation);
            const auto after = inspect_volume_capacity(*image, partition_index, volume->sfs_id, cancellation);
            if (!before)
                return std::unexpected{before.error()};
            if (!after)
                return std::unexpected{after.error()};
            const auto old_profile = std::ranges::find(before->profiles, policy.target, &VolumeCapacityProfile::target);
            const auto new_profile = std::ranges::find(after->profiles, policy.target, &VolumeCapacityProfile::target);
            // Sequence-only replacements do not change reference graphs or replacement defaults.
            // A nonincreasing complete Sequence sum may reduce an already oversized volume.
            if (sequence_replacement_names.contains(entry.name) && old_profile != before->profiles.end() &&
                new_profile != after->profiles.end() && before->object_counts == after->object_counts &&
                old_profile->status == VolumeCapacityStatus::does_not_fit &&
                new_profile->status == VolumeCapacityStatus::does_not_fit && old_profile->minimum_resident_bytes &&
                new_profile->minimum_resident_bytes &&
                *new_profile->minimum_resident_bytes <= *old_profile->minimum_resident_bytes) {
                std::erase_if(destinations,
                              [&](const CapacityDestination &destination) { return destination.name == entry.name; });
                continue;
            }
            // Permit reducing an already oversized source, but do not assume every removal reduces its load peak.
            if (old_profile != before->profiles.end() && new_profile != after->profiles.end() &&
                old_profile->status == VolumeCapacityStatus::does_not_fit &&
                new_profile->status == VolumeCapacityStatus::does_not_fit)
                continue;
            destinations.push_back({partition_index, entry.name, volume->sfs_id});
        }
    }
    return inspect_capacity_destinations(*image, destinations, policy, cancellation);
}
} // namespace axk::detail
