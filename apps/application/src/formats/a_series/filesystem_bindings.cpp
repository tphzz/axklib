#include "formats/a_series/session_state.hpp"

namespace axk::app::formats::a_series {

NativeFilesystemBindings SessionState::filesystem_bindings() const {
    NativeFilesystemBindings bindings;
    bindings.empty_sfs_profile = std::string{profile()};
    for (const auto &[id, object] : snapshots_by_id) {
        if (object->object.format != ObjectFormat::unknown)
            bindings.profile = std::string{profile()};
        bindings.record_objects[{object->partition.value, object->sfs_id.value}].push_back(id);
        bindings.interpretations.emplace(id, object->object.header.raw_type);
    }
    for (const auto &[id, descriptor] : descriptors_by_id)
        bindings.offset_objects[descriptor->data_offset].push_back(id);
    for (const auto &item : content) {
        if (!item.partition_index)
            continue;
        if (item.kind == "partition")
            bindings.partition_scopes.emplace(*item.partition_index, item.id);
        if (item.kind == "volume" && item.volume_directory_id)
            bindings.volume_scopes.emplace(std::pair{*item.partition_index, *item.volume_directory_id}, item.id);
    }
    return bindings;
}

} // namespace axk::app::formats::a_series
